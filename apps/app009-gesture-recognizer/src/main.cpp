// SPDX-License-Identifier: MIT
#include "core.hpp"
#include "events.hpp"
#include "qnn.hpp"
#include "video.hpp"
#include <chrono>
#include <csignal>
#include <deque>
#include <fstream>
#include <gtk/gtk.h>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sys/resource.h>
#include <thread>
using Clock = std::chrono::steady_clock;
static volatile std::sig_atomic_t interrupted = 0;
static double seconds(Clock::time_point t) {
  return std::chrono::duration<double>(Clock::now() - t).count();
}
struct Options {
  std::string input, batch, models, report, events, predictions, dsp;
  bool gui = false, exact = false;
  double duration = 0;
};
struct State {
  std::mutex mutex;
  std::atomic<bool> stop{false}, done{false};
  std::atomic<bool> accuracy_accepted{false};
  Frame latest;
  uint64_t revision = 0;
  std::string caption = "Choose a video file", error;
  std::vector<std::string> event_rows;
  size_t samples = 0, decoded = 0, predicted = 0, dropped = 0, resets = 0;
  double elapsed = 0;
  int window = 0;
  double threshold = 0, release = 0;
  std::vector<double> feature_ms, temporal_ms;
  std::string feature_profile = "[]", temporal_profile = "[]", feature_hash,
              temporal_hash;
};
static std::string hash(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  if (!f)
    throw std::runtime_error("Cannot open model: " + path);
  GChecksum *c = g_checksum_new(G_CHECKSUM_SHA256);
  char b[65536];
  while (f) {
    f.read(b, sizeof(b));
    g_checksum_update(c, reinterpret_cast<guchar *>(b), f.gcount());
  }
  if (!f.eof()) {
    g_checksum_free(c);
    throw std::runtime_error("Cannot read model");
  }
  std::string result = g_checksum_get_string(c);
  g_checksum_free(c);
  return result;
}
static double percentile(std::vector<double> x, double q) {
  if (x.empty())
    return 0;
  std::sort(x.begin(), x.end());
  return x[size_t(std::ceil(q * x.size())) - 1];
}
static void report(const Options &o, State &s) {
  if (o.report.empty())
    return;
  struct rusage r{};
  getrusage(RUSAGE_SELF, &r);
  std::ofstream f(o.report + ".tmp");
  f << std::setprecision(9) << "{\"status\":"
    << ai::quote(!s.error.empty()        ? "FAIL"
                 : s.stop || interrupted ? "CANCELLED"
                                         : "PASS")
    << ",\"error\":" << ai::quote(s.error)
    << ",\"input\":" << ai::quote(o.input)
    << ",\"backend\":\"QNN_HTP\",\"samples\":" << s.samples
    << ",\"model_accuracy_accepted\":"
    << (s.accuracy_accepted ? "true" : "false") << ",\"window\":" << s.window
    << ",\"threshold\":" << s.threshold << ",\"release_seconds\":" << s.release
    << ",\"decoded_frames\":" << s.decoded << ",\"predictions\":" << s.predicted
    << ",\"dropped_frames\":" << s.dropped << ",\"resets\":" << s.resets
    << ",\"elapsed_seconds\":" << s.elapsed
    << ",\"sample_fps\":" << (s.elapsed ? s.samples / s.elapsed : 0)
    << ",\"feature_p95_ms\":" << percentile(s.feature_ms, .95)
    << ",\"temporal_p95_ms\":" << percentile(s.temporal_ms, .95)
    << ",\"peak_rss_kib\":" << r.ru_maxrss
    << ",\"feature_sha256\":" << ai::quote(s.feature_hash)
    << ",\"temporal_sha256\":" << ai::quote(s.temporal_hash)
    << ",\"features_profile\":" << s.feature_profile
    << ",\"temporal_profile\":" << s.temporal_profile << "}\n";
  f.close();
  if (!f)
    throw std::runtime_error("Cannot write report");
  std::filesystem::rename(o.report + ".tmp", o.report);
}
static void run(Options o, State &s) {
  auto start = Clock::now();
  try {
    std::vector<std::string> files;
    if (!o.batch.empty()) {
      std::ifstream f(o.batch);
      if (!f)
        throw std::runtime_error("Cannot read batch list");
      std::string line;
      while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r')
          line.pop_back();
        if (!line.empty())
          files.push_back(
              (std::filesystem::path(o.batch).parent_path() / line).string());
      }
    } else
      files.push_back(o.input);
    if (files.empty())
      throw std::runtime_error("Empty input list");
    for (auto &path : files)
      if (!std::filesystem::is_regular_file(path))
        throw std::runtime_error("Missing input: " + path);
    std::ofstream events, predictions;
    if (!o.events.empty()) {
      events.open(o.events);
      if (!events)
        throw std::runtime_error("Cannot open events output");
    }
    if (!o.predictions.empty()) {
      predictions.open(o.predictions);
      if (!predictions)
        throw std::runtime_error("Cannot open predictions output");
    }
    GKeyFile *config = g_key_file_new();
    GError *error = nullptr;
    if (!g_key_file_load_from_file(config, (o.models + "/model.ini").c_str(),
                                   G_KEY_FILE_NONE, &error)) {
      std::string e = error ? error->message : "Missing model.ini";
      g_clear_error(&error);
      g_key_file_free(config);
      throw std::runtime_error(e);
    }
    auto integer = [&](const char *k) {
      int v = g_key_file_get_integer(config, "gesture", k, &error);
      if (error) {
        g_clear_error(&error);
        throw std::runtime_error(std::string("Invalid model setting ") + k);
      }
      return v;
    };
    auto real = [&](const char *k) {
      double v = g_key_file_get_double(config, "gesture", k, &error);
      if (error) {
        g_clear_error(&error);
        throw std::runtime_error(std::string("Invalid model setting ") + k);
      }
      return v;
    };
    int window = integer("window");
    double threshold = real("threshold"), release = real("release");
    if (g_key_file_has_key(config, "gesture", "accuracy_accepted", nullptr)) {
      s.accuracy_accepted = g_key_file_get_boolean(config, "gesture",
                                                   "accuracy_accepted", &error);
      if (error) {
        g_clear_error(&error);
        g_key_file_free(config);
        throw std::runtime_error("Invalid accuracy acceptance setting");
      }
    }
    g_key_file_free(config);
    if (window != 32 && window != 48)
      throw std::runtime_error("Unsupported temporal window");
    ai::Decoder validate_settings(threshold, release);
    s.window = window;
    s.threshold = threshold;
    s.release = release;
    s.feature_hash = hash(o.models + "/features.bin");
    s.temporal_hash = hash(o.models + "/temporal.bin");
    ai::Qnn features(o.models + "/features.bin", {1, 224, 224, 3}, 576),
        temporal(o.models + "/temporal.bin", {1, 1, unsigned(window), 576}, 12);
    uint64_t loop = 0;
    do {
      bool had_video = false;
      for (const auto &path : files) {
        if (s.stop || interrupted)
          break;
        GError *pixerror = nullptr;
        GdkPixbuf *pix = gdk_pixbuf_new_from_file(path.c_str(), &pixerror);
        g_clear_error(&pixerror);
        if (pix) {
          Frame f;
          f.width = gdk_pixbuf_get_width(pix);
          f.height = gdk_pixbuf_get_height(pix);
          if (f.width <= 0 || f.height <= 0 ||
              uint64_t(f.width) * f.height > 32000000) {
            g_object_unref(pix);
            throw std::runtime_error(
                "Image exceeds 32 megapixel preview limit");
          }
          f.rgb.resize(size_t(f.width) * f.height * 3);
          auto *data = gdk_pixbuf_get_pixels(pix);
          int stride = gdk_pixbuf_get_rowstride(pix),
              channels = gdk_pixbuf_get_n_channels(pix);
          for (int y = 0; y < f.height; ++y)
            for (int x = 0; x < f.width; ++x)
              for (int c = 0; c < 3; ++c)
                f.rgb[(y * f.width + x) * 3 + c] =
                    data[y * stride + x * channels + c];
          g_object_unref(pix);
          std::lock_guard<std::mutex> lock(s.mutex);
          s.latest = std::move(f);
          ++s.revision;
          s.caption = "Image preview: video required for dynamic gestures";
          continue;
        }
        Video video(path, o.gui && !o.exact);
        had_video = true;
        std::deque<std::vector<float>> bank;
        ai::Decoder decoder(threshold, release);
        double previous = -1, last_sample = -1;
        ai::Sampler sampler;
        uint64_t previous_source = 0;
        bool first = true;
        auto flush = [&]() {
          for (const auto &e : decoder.events) {
            std::ostringstream row;
            row << std::setprecision(9) << "{\"input\":" << ai::quote(path)
                << ",\"loop\":" << loop << ",\"label\":" << e.label
                << ",\"class\":" << ai::quote(ai::labels[e.label])
                << ",\"start\":" << e.start << ",\"end\":" << e.end
                << ",\"confirmed_at\":" << e.confirmed
                << ",\"confidence\":" << e.confidence
                << ",\"complete\":" << (e.complete ? "true" : "false") << "}";
            if (events.is_open()) {
              events << row.str() << '\n';
              if (!events)
                throw std::runtime_error("Cannot write event");
            }
            std::lock_guard<std::mutex> lock(s.mutex);
            s.event_rows.push_back(ai::labels[e.label] +
                                   (e.complete ? "" : " (incomplete)"));
            if (s.event_rows.size() > 100)
              s.event_rows.erase(s.event_rows.begin());
          }
          decoder.events.clear();
        };
        while (!s.stop && !interrupted) {
          if (o.duration > 0 && seconds(start) >= o.duration)
            break;
          Frame frame;
          auto result = video.next(frame);
          if (result == Video::Read::Wait)
            continue;
          if (result == Video::Read::End)
            break;
          ++s.decoded;
          if (!first && frame.source_index > previous_source + 1)
            s.dropped += frame.source_index - previous_source - 1;
          previous_source = frame.source_index;
          first = false;
          double t = frame.pts_seconds;
          if (!std::isfinite(t) || t < 0)
            throw std::runtime_error("Invalid source timestamp");
          if (previous >= 0 && (t <= previous || t - previous > .25)) {
            decoder.close(previous, false);
            flush();
            decoder = ai::Decoder(threshold, release);
            bank.clear();
            sampler.reset();
            ++s.resets;
          }
          previous = t;
          if (!sampler.take(t))
            continue;
          last_sample = t;
          auto before = Clock::now();
          auto values = features.execute(ai::preprocess(
              frame.rgb.data(), frame.width, frame.height, frame.width * 3, 3));
          s.feature_ms.push_back(seconds(before) * 1000);
          ++s.samples;
          bank.push_back(std::move(values));
          if (bank.size() > size_t(window))
            bank.pop_front();
          std::string caption = "Collecting motion history (" +
                                std::to_string(bank.size()) + "/" +
                                std::to_string(window) + ")";
          if (bank.size() == size_t(window)) {
            std::vector<float> input;
            input.reserve(window * 576);
            for (auto &x : bank)
              input.insert(input.end(), x.begin(), x.end());
            before = Clock::now();
            auto p = ai::softmax(temporal.execute(input));
            s.temporal_ms.push_back(seconds(before) * 1000);
            ++s.predicted;
            int label = int(std::max_element(p.begin(), p.end()) - p.begin());
            caption =
                (p[label] >= threshold ? ai::labels[label] : "Uncertain") +
                "  " + std::to_string(int(p[label] * 100)) + "%";
            decoder.update(p, t);
            flush();
            if (predictions.is_open()) {
              predictions << std::setprecision(9)
                          << "{\"input\":" << ai::quote(path)
                          << ",\"loop\":" << loop
                          << ",\"source_frame\":" << frame.source_index
                          << ",\"pts\":" << t << ",\"probabilities\":[";
              for (size_t i = 0; i < p.size(); ++i) {
                if (i)
                  predictions << ',';
                predictions << p[i];
              }
              predictions << "]}\n";
              if (!predictions)
                throw std::runtime_error("Cannot write prediction");
            }
          }
          {
            std::lock_guard<std::mutex> lock(s.mutex);
            s.latest = std::move(frame);
            s.caption = caption;
            ++s.revision;
          }
        }
        decoder.close(last_sample < 0 ? 0 : last_sample + .1, false);
        flush();
      }
      ++loop;
      if (!had_video)
        break;
    } while (o.duration > 0 && seconds(start) < o.duration && !s.stop &&
             !interrupted);
    if (events.is_open()) {
      events.close();
      if (!events)
        throw std::runtime_error("Cannot flush events");
    }
    if (predictions.is_open()) {
      predictions.close();
      if (!predictions)
        throw std::runtime_error("Cannot flush predictions");
    }
    s.feature_profile = features.profile_json();
    s.temporal_profile = temporal.profile_json();
  } catch (const std::exception &e) {
    s.error = e.what();
  }
  s.elapsed = seconds(start);
  try {
    report(o, s);
  } catch (const std::exception &e) {
    s.error = e.what();
  }
  s.done = true;
}
struct Ui {
  Options options;
  std::unique_ptr<State> state;
  std::thread worker;
  GtkWidget *window = nullptr, *image = nullptr, *caption = nullptr,
            *quality = nullptr, *history = nullptr, *start = nullptr,
            *stop = nullptr, *open = nullptr;
  uint64_t revision = 0;
  ~Ui() {
    if (state)
      state->stop = true;
    if (worker.joinable())
      worker.join();
  }
  void begin() {
    if (worker.joinable()) {
      if (!state->done)
        return;
      worker.join();
    }
    state = std::make_unique<State>();
    revision = 0;
    gtk_label_set_text(GTK_LABEL(history), "");
    gtk_widget_set_sensitive(start, false);
    gtk_widget_set_sensitive(open, false);
    gtk_widget_set_sensitive(stop, true);
    worker = std::thread(run, options, std::ref(*state));
  }
  void create() {
    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Dynamic Gesture Recognizer");
    gtk_window_set_default_size(GTK_WINDOW(window), 900, 480);
    gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
    auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 16);
    gtk_container_add(GTK_CONTAINER(window), box);
    auto *title = gtk_label_new("Dynamic Gesture Recognition - 11 Actions");
    gtk_box_pack_start(GTK_BOX(box), title, false, false, 0);
    quality = gtk_label_new("Validation model: accuracy not accepted");
    gtk_box_pack_start(GTK_BOX(box), quality, false, false, 0);
    auto *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(box), bar, false, false, 0);
    open = gtk_button_new_with_label("Open file...");
    start = gtk_button_new_with_label("Start");
    stop = gtk_button_new_with_label("Stop");
    for (auto *b : {open, start, stop})
      gtk_box_pack_start(GTK_BOX(bar), b, false, false, 0);
    gtk_widget_set_sensitive(stop, false);
    caption = gtk_label_new("Choose a video file");
    gtk_label_set_ellipsize(GTK_LABEL(caption), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(box), caption, false, false, 0);
    image = gtk_image_new();
    gtk_box_pack_start(GTK_BOX(box), image, true, true, 0);
    history = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(history), PANGO_ELLIPSIZE_START);
    gtk_box_pack_start(GTK_BOX(box), history, false, false, 0);
    g_signal_connect(window, "destroy",
                     G_CALLBACK(+[](GtkWidget *, gpointer p) {
                       auto *u = static_cast<Ui *>(p);
                       if (u->state)
                         u->state->stop = true;
                       gtk_main_quit();
                     }),
                     this);
    g_signal_connect(start, "clicked", G_CALLBACK(+[](GtkButton *, gpointer p) {
                       static_cast<Ui *>(p)->begin();
                     }),
                     this);
    g_signal_connect(stop, "clicked", G_CALLBACK(+[](GtkButton *, gpointer p) {
                       auto *u = static_cast<Ui *>(p);
                       if (u->state)
                         u->state->stop = true;
                     }),
                     this);
    g_signal_connect(
        open, "clicked", G_CALLBACK(+[](GtkButton *, gpointer p) {
          auto *u = static_cast<Ui *>(p);
          auto *dialog = gtk_file_chooser_dialog_new(
              "Open gesture video", GTK_WINDOW(u->window),
              GTK_FILE_CHOOSER_ACTION_OPEN, "Cancel", GTK_RESPONSE_CANCEL,
              "Open", GTK_RESPONSE_ACCEPT, nullptr);
          if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
            char *file =
                gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
            u->options.input = file;
            g_free(file);
            gtk_widget_destroy(dialog);
            u->begin();
          } else
            gtk_widget_destroy(dialog);
        }),
        this);
    g_timeout_add(
        100,
        +[](gpointer p) -> gboolean {
          auto *u = static_cast<Ui *>(p);
          if (interrupted) {
            if (u->state)
              u->state->stop = true;
            gtk_main_quit();
            return false;
          }
          if (!u->state)
            return true;
          auto &s = *u->state;
          std::lock_guard<std::mutex> lock(s.mutex);
          gtk_label_set_text(GTK_LABEL(u->quality),
                             s.accuracy_accepted
                                 ? "Model accuracy accepted"
                                 : "Validation model: accuracy not accepted");
          if (s.done) {
            gtk_widget_set_sensitive(u->start, true);
            gtk_widget_set_sensitive(u->open, true);
            gtk_widget_set_sensitive(u->stop, false);
            if (!s.error.empty())
              gtk_label_set_text(GTK_LABEL(u->caption), s.error.c_str());
          }
          if (s.revision != u->revision) {
            u->revision = s.revision;
            auto &f = s.latest;
            auto *pix = gdk_pixbuf_new_from_data(
                f.rgb.data(), GDK_COLORSPACE_RGB, false, 8, f.width, f.height,
                f.width * 3, nullptr, nullptr);
            double scale = std::min(800.0 / f.width, 270.0 / f.height);
            auto *resized = gdk_pixbuf_scale_simple(pix, int(f.width * scale),
                                                    int(f.height * scale),
                                                    GDK_INTERP_BILINEAR);
            gtk_image_set_from_pixbuf(GTK_IMAGE(u->image), resized);
            g_object_unref(resized);
            g_object_unref(pix);
            gtk_label_set_text(GTK_LABEL(u->caption), s.caption.c_str());
            std::string text;
            for (size_t i = s.event_rows.size() > 3 ? s.event_rows.size() - 3
                                                    : 0;
                 i < s.event_rows.size(); ++i) {
              if (!text.empty())
                text += " | ";
              text += s.event_rows[i];
            }
            gtk_label_set_text(GTK_LABEL(u->history), text.c_str());
          }
          return true;
        },
        this);
    gtk_widget_show_all(window);
    if (!options.input.empty())
      begin();
    gtk_main();
    if (state)
      state->stop = true;
    if (worker.joinable())
      worker.join();
  }
};
int main(int argc, char **argv) {
  Options o;
  try {
    auto exe = std::filesystem::canonical("/proc/self/exe");
    o.models =
        (exe.parent_path().parent_path() / "share/gesture-recognizer/models")
            .string();
    for (int i = 1; i < argc; ++i) {
      std::string key = argv[i];
      if (key == "--gui") {
        o.gui = true;
        continue;
      }
      if (key == "--exact") {
        o.exact = true;
        continue;
      }
      if (key == "--self-test") {
        ai::Decoder d;
        auto p = ai::softmax(std::vector<float>(12, 0));
        if (p.size() != 12)
          throw std::runtime_error("Self-test failed");
        std::cout << "Gesture core self-test PASS\n";
        return 0;
      }
      if (key == "--help") {
        std::cout << "gesture-recognizer [--gui] --input FILE [--batch LIST] "
                     "[--model-dir DIR] [--events JSONL] [--predictions JSONL] "
                     "[--report JSON] [--duration SECONDS] [--exact]\n";
        return 0;
      }
      if (i + 1 >= argc)
        throw std::runtime_error("Missing argument");
      std::string value = argv[++i];
      if (key == "--input")
        o.input = value;
      else if (key == "--batch")
        o.batch = value;
      else if (key == "--model-dir")
        o.models = value;
      else if (key == "--report")
        o.report = value;
      else if (key == "--events")
        o.events = value;
      else if (key == "--predictions")
        o.predictions = value;
      else if (key == "--dsp-dir")
        o.dsp = value;
      else if (key == "--duration") {
        size_t n;
        o.duration = std::stod(value, &n);
        if (n != value.size() || !std::isfinite(o.duration) || o.duration < 0 ||
            o.duration > 86400)
          throw std::runtime_error("Invalid duration");
      } else
        throw std::runtime_error("Unknown option " + key);
    }
    for (auto *p : {&o.input, &o.batch, &o.models, &o.report, &o.events,
                    &o.predictions, &o.dsp})
      if (!p->empty())
        *p = std::filesystem::absolute(*p).string();
    if (!o.dsp.empty())
      std::filesystem::current_path(o.dsp);
    if (!o.batch.empty() && (o.gui || !o.input.empty()))
      throw std::runtime_error("Batch is CLI-only and excludes input");
    std::signal(SIGINT, +[](int) { interrupted = 1; });
    std::signal(SIGTERM, +[](int) { interrupted = 1; });
    gst_init(nullptr, nullptr);
    if (o.gui) {
      if (!gtk_init_check(nullptr, nullptr))
        throw std::runtime_error("Cannot connect to desktop");
      Ui ui;
      ui.options = o;
      ui.create();
      return ui.state && !ui.state->error.empty() ? 1 : 0;
    }
    State s;
    run(o, s);
    if (!s.error.empty()) {
      std::cerr << s.error << '\n';
      return 1;
    }
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}

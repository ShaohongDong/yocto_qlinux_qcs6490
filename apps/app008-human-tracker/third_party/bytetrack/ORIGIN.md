Source: https://github.com/Vertical-Beach/ByteTrack-cpp
Commit: d43805d461a714f65da039981bd5f5d21cf5cf59
License: MIT (see LICENSE).

Local changes: detection_index propagated through both association stages for exact keypoint ownership; remove expired tracks immediately and bound removed-track history. Refresh cached rectangles after Kalman prediction so association uses predicted locations; optional confidence-score fusion for first and tentative association stages (low-score recovery remains IoU-only). Line endings normalized on edited files.

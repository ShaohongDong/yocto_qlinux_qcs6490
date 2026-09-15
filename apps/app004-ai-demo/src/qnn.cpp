// SPDX-License-Identifier: MIT
#include "qnn.hpp"
#include "core.hpp"
#include <QnnInterface.h>
#include <System/QnnSystemInterface.h>
#include <dlfcn.h>
#include <cstring>
#include <fstream>
#include <sstream>
#include <cstdio>

namespace ai {
namespace {
void check(Qnn_ErrorHandle_t status, const char* operation) {
    if (status != QNN_SUCCESS) throw std::runtime_error(std::string(operation)+" failed: QNN error "+std::to_string(status)+" (NPU required)");
}
template<class T> T symbol(void* library, const char* name) {
    auto value=reinterpret_cast<T>(dlsym(library,name));
    if (!value) throw std::runtime_error(std::string("Missing QNN symbol: ")+name);
    return value;
}
void* library(const char* name) {
    void* result=dlopen(name,RTLD_NOW|RTLD_LOCAL);
    if (!result) throw std::runtime_error(std::string("Cannot load ")+name+": "+dlerror());
    return result;
}
#define FIELD(t, f) ((t).version==QNN_TENSOR_VERSION_1 ? (t).v1.f : (t).v2.f)
size_t elements(const Qnn_Tensor_t& t) {
    if (t.version!=QNN_TENSOR_VERSION_1 && t.version!=QNN_TENSOR_VERSION_2) throw std::runtime_error("Unsupported tensor version");
    size_t n=1;
    for (uint32_t i=0; i<FIELD(t,rank); ++i) {
        auto d=FIELD(t,dimensions)[i];
        if (!d || d>10000000 || n>10000000/d) throw std::runtime_error("Invalid tensor shape");
        n*=d;
    }
    return n;
}
size_t bytes(const Qnn_Tensor_t& t) {
    auto type=FIELD(t,dataType);
    if(type==QNN_DATATYPE_FLOAT_32) return elements(t)*4;
    if(type==QNN_DATATYPE_UFIXED_POINT_8) return elements(t);
    if(type==QNN_DATATYPE_UFIXED_POINT_16) return elements(t)*2;
    throw std::runtime_error("Unsupported model I/O dtype");
}
Qnn_ScaleOffset_t encoding(const Qnn_Tensor_t& t) {
    const auto& q=FIELD(t,quantizeParams);
    if (q.quantizationEncoding!=QNN_QUANTIZATION_ENCODING_SCALE_OFFSET || !(q.scaleOffsetEncoding.scale>0))
        throw std::runtime_error("Model I/O requires per-tensor scale-offset quantization");
    return q.scaleOffsetEncoding;
}
}
struct Qnn::Impl {
    void *htp=nullptr, *system=nullptr;
    QNN_INTERFACE_VER_TYPE api{};
    QNN_SYSTEM_INTERFACE_VER_TYPE sys{};
    Qnn_BackendHandle_t backend=nullptr;
    Qnn_DeviceHandle_t device=nullptr;
    Qnn_ContextHandle_t context=nullptr;
    Qnn_GraphHandle_t graph=nullptr;
    Qnn_ProfileHandle_t profile=nullptr;
    QnnSystemContext_Handle_t metadata=nullptr;
    Qnn_Tensor_t input=QNN_TENSOR_INIT, output=QNN_TENSOR_INIT;
    std::vector<uint8_t> binary, in, out;
    bool last=false;
    ~Impl() {
        if(context) api.contextFree(context,nullptr);
        if(profile) api.profileFree(profile);
        if(device) api.deviceFree(device);
        if(backend) api.backendFree(backend);
        if(metadata) sys.systemContextFree(metadata);
        if(system) dlclose(system);
        if(htp) dlclose(htp);
    }
    void load(const std::string& path) {
        std::ifstream file(path,std::ios::binary|std::ios::ate);
        if(!file) throw std::runtime_error("Cannot open model context: "+path);
        auto size=file.tellg();
        if(size<=0 || size>512*1024*1024) throw std::runtime_error("Invalid model size");
        binary.resize(size); file.seekg(0); file.read(reinterpret_cast<char*>(binary.data()),size);
        if(!file) throw std::runtime_error("Truncated model context");
        htp=library("libQnnHtp.so"); system=library("libQnnSystem.so");
        const QnnInterface_t** providers=nullptr; uint32_t count=0;
        check(symbol<decltype(&QnnInterface_getProviders)>(htp,"QnnInterface_getProviders")(&providers,&count),"getProviders");
        bool found=false;
        for(uint32_t i=0;i<count;++i) if(providers[i]->apiVersion.coreApiVersion.major==QNN_API_VERSION_MAJOR && providers[i]->apiVersion.coreApiVersion.minor>=QNN_API_VERSION_MINOR) {
            api=providers[i]->QNN_INTERFACE_VER_NAME; found=true; break;
        }
        if(!found) throw std::runtime_error("Incompatible QNN HTP API version");
        const QnnSystemInterface_t** systems=nullptr;
        check(symbol<decltype(&QnnSystemInterface_getProviders)>(system,"QnnSystemInterface_getProviders")(&systems,&count),"system providers");
        found=false;
        for(uint32_t i=0;i<count;++i) if(systems[i]->systemApiVersion.major==QNN_SYSTEM_API_VERSION_MAJOR && systems[i]->systemApiVersion.minor>=QNN_SYSTEM_API_VERSION_MINOR) {
            sys=systems[i]->QNN_SYSTEM_INTERFACE_VER_NAME; found=true; break;
        }
        if(!found) throw std::runtime_error("Incompatible QNN System API version");
        check(api.backendCreate(nullptr,nullptr,&backend),"HTP backendCreate");
        check(api.deviceCreate(nullptr,nullptr,&device),"HTP deviceCreate");
        check(api.profileCreate(backend,QNN_PROFILE_LEVEL_BASIC,&profile),"profileCreate");
        check(sys.systemContextCreate(&metadata),"systemContextCreate");
        const QnnSystemContext_BinaryInfo_t* info=nullptr; Qnn_ContextBinarySize_t info_size=0;
        check(sys.systemContextGetBinaryInfo(metadata,binary.data(),binary.size(),&info,&info_size),"model metadata");
        QnnSystemContext_GraphInfo_t* graphs=nullptr;
        uint32_t n=0;
        switch(info->version) {
        case QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_1: graphs=info->contextBinaryInfoV1.graphs; n=info->contextBinaryInfoV1.numGraphs; break;
        case QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_2: graphs=info->contextBinaryInfoV2.graphs; n=info->contextBinaryInfoV2.numGraphs; break;
        case QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_3: graphs=info->contextBinaryInfoV3.graphs; n=info->contextBinaryInfoV3.numGraphs; break;
        default: throw std::runtime_error("Unsupported context metadata version");
        }
        if(n!=1 || !graphs) throw std::runtime_error("Expected one YOLO graph");
        std::string name;
        auto capture=[&](const auto& g) {
            if(g.numGraphInputs!=1 || g.numGraphOutputs!=1) throw std::runtime_error("Expected one input and one YOLO output");
            input=g.graphInputs[0]; output=g.graphOutputs[0]; name=g.graphName;
        };
        switch(graphs[0].version) {
        case QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_1: capture(graphs[0].graphInfoV1); break;
        case QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_2: capture(graphs[0].graphInfoV2); break;
        case QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_3: capture(graphs[0].graphInfoV3); break;
        default: throw std::runtime_error("Unsupported graph metadata version");
        }
        if(elements(input)!=640*640*3 || FIELD(input,rank)!=4 || FIELD(input,dimensions)[1]!=640 || FIELD(input,dimensions)[2]!=640 || FIELD(input,dimensions)[3]!=3)
            throw std::runtime_error("Expected NHWC input [1,640,640,3]");
        if(elements(output)!=84*8400 || FIELD(output,rank)!=3 || FIELD(output,dimensions)[0]!=1)
            throw std::runtime_error("Unexpected YOLO output");
        last=FIELD(output,dimensions)[2]==84;
        if(!last && FIELD(output,dimensions)[1]!=84) throw std::runtime_error("Unexpected output layout");
        in.resize(bytes(input)); out.resize(bytes(output));
        FIELD(input,memType)=QNN_TENSORMEMTYPE_RAW; FIELD(output,memType)=QNN_TENSORMEMTYPE_RAW;
        FIELD(input,clientBuf).data=in.data(); FIELD(input,clientBuf).dataSize=uint32_t(in.size());
        FIELD(output,clientBuf).data=out.data(); FIELD(output,clientBuf).dataSize=uint32_t(out.size());
        check(api.contextCreateFromBinary(backend,device,nullptr,binary.data(),binary.size(),&context,profile),"HTP contextCreateFromBinary");
        check(api.graphRetrieve(context,name.c_str(),&graph),"graphRetrieve");
        std::fprintf(stderr,"NPU backend: libQnnHtp.so; graph=%s; input_type=%u; output_type=%u\n",name.c_str(),unsigned(FIELD(input,dataType)),unsigned(FIELD(output,dataType)));
    }
};
Qnn::Qnn(const std::string& model):impl(std::make_unique<Impl>()) {impl->load(model);}
Qnn::~Qnn()=default;
bool Qnn::channels_last() const {return impl->last;}
std::vector<float> Qnn::execute(const std::vector<float>& nhwc) {
    auto& p=*impl;
    if(nhwc.size()!=elements(p.input)) throw std::runtime_error("Input size mismatch");
    auto dtype=FIELD(p.input,dataType);
    if(dtype==QNN_DATATYPE_FLOAT_32) std::memcpy(p.in.data(),nhwc.data(),p.in.size());
    else {
        auto q=encoding(p.input);
        for(size_t i=0;i<nhwc.size();++i) {
            auto v=quantize(nhwc[i],q.scale,q.offset,dtype==QNN_DATATYPE_UFIXED_POINT_8 ? 255:65535);
            if(dtype==QNN_DATATYPE_UFIXED_POINT_8) p.in[i]=uint8_t(v);
            else {uint16_t value=uint16_t(v); std::memcpy(p.in.data()+2*i,&value,2);}
        }
    }
    check(p.api.graphExecute(p.graph,&p.input,1,&p.output,1,p.profile,nullptr),"HTP graphExecute");
    std::vector<float> values(elements(p.output));
    dtype=FIELD(p.output,dataType);
    if(dtype==QNN_DATATYPE_FLOAT_32) std::memcpy(values.data(),p.out.data(),p.out.size());
    else {
        auto q=encoding(p.output);
        for(size_t i=0;i<values.size();++i) {
            uint16_t value=p.out[i];
            if(dtype==QNN_DATATYPE_UFIXED_POINT_16) std::memcpy(&value,p.out.data()+2*i,2);
            values[i]=(int32_t(value)+q.offset)*q.scale;
        }
    }
    return values;
}
std::string Qnn::profile_json() const {
    const auto& p=*impl;
    const QnnProfile_EventId_t* events=nullptr; uint32_t count=0;
    check(p.api.profileGetEvents(p.profile,&events,&count),"profileGetEvents");
    std::ostringstream json; json<<"["; bool first=true;
    auto emit=[&](auto&& self,QnnProfile_EventId_t id,int depth)->void {
        if(depth>8) return;
        QnnProfile_EventData_t data{};
        check(p.api.profileGetEventData(id,&data),"profileGetEventData");
        if(!first) json<<",";
        first=false;
        // Identifiers are diagnostic strings; retain only safe printable characters.
        std::string name=data.identifier ? data.identifier : "";
        for(char& c:name) if(c<' ' || c=='"' || c=='\\') c='_';
        json<<"{\"type\":"<<data.type<<",\"unit\":"<<data.unit<<",\"value\":"<<data.value<<",\"name\":\""<<name<<"\"}";
        const QnnProfile_EventId_t* children=nullptr; uint32_t n=0;
        check(p.api.profileGetSubEvents(id,&children,&n),"profileGetSubEvents");
        for(uint32_t i=0;i<n;++i) self(self,children[i],depth+1);
    };
    for(uint32_t i=0;i<count;++i) emit(emit,events[i],0);
    json<<"]"; return json.str();
}
}

#define XR_USE_PLATFORM_ANDROID
#include <jni.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <sstream>
#include <vector>

extern "C" JNIEXPORT jstring JNICALL Java_com_pinpin_wallscreen_probe_MainActivity_inspect(JNIEnv* env,jclass,jobject context) {
    JavaVM* vm=nullptr;env->GetJavaVM(&vm);
    PFN_xrInitializeLoaderKHR initialize=nullptr;
    XrResult resolve=xrGetInstanceProcAddr(XR_NULL_HANDLE,"xrInitializeLoaderKHR",reinterpret_cast<PFN_xrVoidFunction*>(&initialize));
    XrResult init=XR_ERROR_INITIALIZATION_FAILED;
    if(XR_SUCCEEDED(resolve)&&initialize){
        XrLoaderInitInfoAndroidKHR info{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
        info.applicationVM=vm;info.applicationContext=context;
        init=initialize(reinterpret_cast<XrLoaderInitInfoBaseHeaderKHR*>(&info));
    }
    uint32_t count=0;
    XrResult result=xrEnumerateInstanceExtensionProperties(nullptr,0,&count,nullptr);
    std::vector<XrExtensionProperties> extensions(count,{XR_TYPE_EXTENSION_PROPERTIES});
    if(XR_SUCCEEDED(result))result=xrEnumerateInstanceExtensionProperties(nullptr,count,&count,extensions.data());
    std::ostringstream json;
    json<<"{\"loaderInit\":"<<init<<",\"enumerate\":"<<result<<",\"extensions\":[";
    if(XR_SUCCEEDED(result))for(uint32_t i=0;i<count;i++){
        if(i)json<<',';
        json<<'\"'<<extensions[i].extensionName<<'\"';
    }
    json<<"]}";
    return env->NewStringUTF(json.str().c_str());
}

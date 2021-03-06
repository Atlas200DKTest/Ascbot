#include <libgen.h>
#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <thread>

#include "BatchImageParaWithScale.h"
#include "HiRoadFollowing.h"
#include "hiaiengine/api.h"
#include "log.h"

#define LOG_SWITCH 0

static const uint32_t GRAPH_ID = 91282072;

namespace hi {

class HiRFImpl : public hiai::DataRecvInterface {
public:
    HiRFImpl();
    explicit HiRFImpl(const std::string& filename);
    virtual ~HiRFImpl();
    /**
    * @ingroup FasterRcnnDataRecvInterface
    * @brief RecvData RecvData
    * @param [in]
    */
    HIAI_StatusT RecvData(const std::shared_ptr<void>& message);

    /**
     * @brief init and start hi engine graph.
     * @param [in] graph_config graph config file path.
     * */
    HIAI_StatusT HIAI_InitAndStartGraph(std::string graph_config);

    /**
     * @brief wrap ImageInfo use image data.
     * */
    HIAI_StatusT makeImageInfo(NewImageParaT* imgData, const HIImgData& input);

    bool init(const std::string& config_path);

    bool deInit();

    void setCallbackFunction(rf_callback_t func);

    bool predict(const HIImgData& frame);

    bool isInit() const;

private:
    std::shared_ptr<hiai::Graph> graph;
    hiai::EnginePortID engine_id;
    //! Callback function handle.
    rf_callback_t callback_func;
    //!
    std::string file_name_;
    //! Frame number
    int64_t mFrameNum = 0;
    bool isInit_ = false;
};

HiRFImpl::HiRFImpl() {
}

HiRFImpl::HiRFImpl(const std::string& filename) : file_name_(filename) {
}

HiRFImpl::~HiRFImpl() {
}

// Init and create graph
HIAI_StatusT HiRFImpl::HIAI_InitAndStartGraph(std::string graph_config) {
    // Step1: Global System Initialization before using HIAI Engine
    HIAI_StatusT status = HIAI_Init(0);

    // Step2: Create and Start the Graph
    status = hiai::Graph::CreateGraph(graph_config);
    if (status != HIAI_OK) {
        HIAI_ENGINE_LOG(status, "Fail to start graph");
        return status;
    }

    // Step3
    std::shared_ptr<hiai::Graph> graph = hiai::Graph::GetInstance(GRAPH_ID);
    if (nullptr == graph) {
        HIAI_ENGINE_LOG("Fail to get the graph-%u", GRAPH_ID);
        return status;
    }
    int leaf_array[1] = {301};  // leaf node id

    for (int i = 0; i < 1; i++) {
        hiai::EnginePortID target_port_config;
        target_port_config.graph_id = GRAPH_ID;
        target_port_config.engine_id = leaf_array[i];
        target_port_config.port_id = 0;
        graph->SetDataRecvFunctor(target_port_config, std::shared_ptr<HiRFImpl>(this));
    }
    return HIAI_OK;
}

/**
* @ingroup HiRFImpl
* @brief RecvData RecvData
* @param [in]
*/
HIAI_StatusT HiRFImpl::RecvData(const std::shared_ptr<void>& message) {
    std::shared_ptr<RFResultT> data = std::static_pointer_cast<RFResultT>(message);
    std::vector<RFData> result;
    float org_width = 1.0f;
    float org_height = 1.0f;
    if (0 != data->width && 0 != data->height && 0.0f != data->scale_width &&
        0.0f != data->scale_height) {
        org_width = data->width / data->scale_width;
        org_height = data->height / data->scale_height;
    } else {
        LOGE("invalid output data width,height[%d,%d], scale[%.5f,%.5f]", data->width, data->height,
             data->scale_width, data->scale_height);
    }
    int64_t timestamp = data->timestamp;

    for (int i = 0; i < data->data.size(); i++) {
        RFData rf;
        rf.raw_x = data->data.at(0).start;
        rf.raw_y = data->data.at(0).end;
        rf.x = rf.raw_x * org_width;
        rf.y = rf.raw_y * org_height;
        double atanx = (rf.x - org_width / 2) / (org_height - rf.y);
        rf.angle = atan(atanx) * 180 / M_PI;
        rf.timestamp = timestamp;
        result.push_back(rf);
        // LOGD_IF(LOG_SWITCH, "SDK[RF] raw[%.5f,%.5f] xy[%.5f,%.5f] angle=%.5f, timestamp=%ld",
        //    rf.raw_x, rf.raw_y, rf.x, rf.y, rf.angle, rf.timestamp);
    }
    if (callback_func) {
        callback_func(result);
    } else {
        LOGE("call back func is null");
    }
    return HIAI_OK;
}

bool HiRFImpl::init(const std::string& config_path) {
    LOGI("init");
    HIAI_StatusT ret = HIAI_OK;
    // 1.create graph
    ret = HIAI_InitAndStartGraph(config_path);
    if (HIAI_OK != ret) {
        LOGE("Fail to start graph");
        HIAI_ENGINE_LOG("Fail to start graph");
        return STATE_OPERATION_FAILED;
    }

    // 2.send data
    graph = hiai::Graph::GetInstance(GRAPH_ID);
    if (nullptr == graph) {
        LOGE("Fail to get the graph-%u", GRAPH_ID);
        HIAI_ENGINE_LOG("Fail to get the graph-%u", GRAPH_ID);
        return STATE_OPERATION_FAILED;
    }

    // send data to SourceEngine 0 port
    engine_id.graph_id = GRAPH_ID;
    engine_id.engine_id = 467;
    engine_id.port_id = 0;
    if (nullptr != graph) {
        isInit_ = true;
    }
    return STATE_SUCCESS;
}

bool HiRFImpl::deInit() {
    LOGI("deInit");
    if (nullptr != graph) {
        hiai::Graph::DestroyGraph(GRAPH_ID);
    }
    return false;
}

void HiRFImpl::setCallbackFunction(rf_callback_t func) {
    callback_func = func;
}

bool HiRFImpl::predict(const HIImgData& frame) {
    if (!isInit()) {
        return STATE_INVALID_OPERATION;
    }
    // assert
    if (0 == frame.width() || 0 == frame.height() || nullptr == frame.data()) {
        LOGW("HiRFImpl invalid input buff[%d,%d,%p]!", frame.width(), frame.height(), frame.data());
        return false;
    }

    std::shared_ptr<BatchImageParaWithScaleT> imageInfoBatch =
        std::make_shared<BatchImageParaWithScaleT>();
    if (imageInfoBatch == NULL) {
        HIAI_ENGINE_LOG(
            HIAI_IDE_ERROR,
            "[Mind_road_following_dataset] make shared for BatchImageParaWithScaleT error!");
        return HIAI_ERROR;
    }
    int frameId = 0;
    int batchId = 0;
    int batchNum = 1;
    NewImageParaT imgData;
    HIAI_StatusT ret = makeImageInfo(&imgData, frame);
    if (HIAI_OK != ret) {
        HIAI_ENGINE_LOG(HIAI_IDE_ERROR,
                        "[Mind_road_following_dataset] Error: make image info frame id %u for "
                        "batch id %u failed! Stop to send images!",
                        frameId, batchId);
        return ret;
    }
    mFrameNum += 1;
    // LOGD_IF(LOG_SWITCH, "frame=%ld, w=%d, h=%d, timestamp=%ld", mFrameNum, imgData.img.width,
    //    imgData.img.height, imgData.timestamp);
    // LOGI("width:%d, height:%d, size=%d", imgData.img.width, imgData.img.height,
    // imgData.img.size);
    imageInfoBatch->v_img.push_back(imgData);
    imageInfoBatch->b_info.frame_ID.push_back(0);
    // then send data
    HIAI_StatusT hiai_ret = HIAI_OK;
    imageInfoBatch->b_info.batch_size = imageInfoBatch->v_img.size();
    imageInfoBatch->b_info.max_batch_size = 1;
    imageInfoBatch->b_info.batch_ID = batchId;
    imageInfoBatch->b_info.is_first = (batchId == 0 ? true : false);
    imageInfoBatch->b_info.is_last = (batchId == batchNum - 1 ? true : false);

    // do{
    graph->SendData(engine_id, "BatchImageParaWithScaleT",
                    std::static_pointer_cast<void>(imageInfoBatch));
    return true;
}

HIAI_StatusT HiRFImpl::makeImageInfo(NewImageParaT* imgData, const HIImgData& input) {
    // if(index < 0 || index >= dataset_info_.size()){
    //     return HIAI_ERROR;
    // }
    int32_t img_size = 0;
    if (input.type() == TYPE_YUV_NV12) {
        imgData->img.format = (IMAGEFORMAT)IMAGE_TYPE_NV12;
        img_size = input.width() * input.height() * 1.5;
    } else {  // otherwise only support jpeg format.
        imgData->img.format = (IMAGEFORMAT)IMAGE_TYPE_JPEG;
    }
    imgData->img.width = input.width();
    imgData->img.height = input.height();
    imgData->timestamp = input.timestamp();
    // std::string imageFullPath = input.file;

    uint8_t* imageBufferPtr = NULL;  // input.data();
    // run on same side with dvpp
    if ((ImageType)imgData->img.format == IMAGE_TYPE_JPEG) {
        // transfer jepg to imagepreprocess use dvpp jepgd need to add 8 bit for check
        imgData->img.size = img_size + 8;
    } else {
        imgData->img.size = img_size;
    }
    // run on same side with dvpp need to make the mem align to 128(dvpp need)
    imageBufferPtr = reinterpret_cast<uint8_t*>(memalign(128, imgData->img.size));

    if (imageBufferPtr == NULL) {
        LOGW("input buffer is null!");
        HIAI_ENGINE_LOG(HIAI_IDE_ERROR,
                        "[Mind_road_following_dataset] alloc buffer error in makeImageInfo");
        return HIAI_ERROR;
    }
    int ret = memcpy_s(imageBufferPtr, imgData->img.size, input.data(), imgData->img.size);
    // std::cout << "img path:" << imageFullPath.c_str() << ", img size:" << img_size << std::endl;
    // bool ret = GetImageBuffer(imageFullPath.c_str(), imageBufferPtr, img_size, 0);

    // if(!ret){
    //     delete[] imageBufferPtr;
    //     imageBufferPtr = NULL;
    //     return HIAI_ERROR;
    // }
    std::shared_ptr<uint8_t> data(imageBufferPtr);
    imgData->img.data = data;
    return HIAI_OK;
}

bool HiRFImpl::isInit() const {
    return isInit_;
}

HiRoadFollowing::HiRoadFollowing() {
    impl = new HiRFImpl();
}

HiRoadFollowing::~HiRoadFollowing() {
    if (nullptr != impl) {
        delete reinterpret_cast<HiRFImpl*>(impl);
        impl = nullptr;
    }
}

bool HiRoadFollowing::init(const std::string& config_path) {
    if (nullptr != impl) {
        return (reinterpret_cast<HiRFImpl*>(impl))->init(config_path);
    } else {
        LOGE("HiRoadFollowing::init can not init!");
        return false;
    }
}

bool HiRoadFollowing::deInit() {
    if (nullptr != impl && isInit()) {
        return (reinterpret_cast<HiRFImpl*>(impl))->deInit();
    } else {
        LOGE("HiRoadFollowing::deInit can not init!");
        return false;
    }
}

void HiRoadFollowing::setCallbackFunction(rf_callback_t func) {
    if (nullptr != impl && isInit()) {
        return (reinterpret_cast<HiRFImpl*>(impl))->setCallbackFunction(func);
    } else {
        LOGE("HiRoadFollowing::setCallbackFunction can not init!");
        return;
    }
}

bool HiRoadFollowing::predict(const HIImgData& frame) {
    if (nullptr != impl && isInit()) {
        return (reinterpret_cast<HiRFImpl*>(impl))->predict(frame);
    } else {
        LOGE("HiRoadFollowing::predict can not init!");
        return false;
    }
}

bool HiRoadFollowing::isInit() const {
    if (nullptr != impl) {
        return (reinterpret_cast<HiRFImpl*>(impl))->isInit();
    } else {
        LOGE("HiRoadFollowing::isInit can not init!");
        return false;
    }
}

}  // namespace hi

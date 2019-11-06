#include <libgen.h>
#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <thread>

#include "BatchImageParaWithScale.h"
#include "HiCollisionAvoidance.h"
#include "hiaiengine/api.h"
#include "log.h"

static const uint32_t GRAPH_ID = 86391820;
#define SRC_ENGINE_ID 767
#define DEST_ENGINE_ID 284

namespace hi {

class HiCollisionImpl : public hiai::DataRecvInterface {
public:
    HiCollisionImpl();
    explicit HiCollisionImpl(const std::string& filename);
    virtual ~HiCollisionImpl();
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

    void setCallbackFunction(ca_callback_t func);

    bool predict(const HIImgData& frame);

    bool isInit() const;

private:
    std::shared_ptr<hiai::Graph> graph;
    hiai::EnginePortID engine_id;
    //! Callback function handle.
    ca_callback_t callback_func;
    //!
    std::string file_name_;
    bool isInit_ = false;
};

HiCollisionImpl::HiCollisionImpl() {
}

HiCollisionImpl::HiCollisionImpl(const std::string& filename) : file_name_(filename) {
}

HiCollisionImpl::~HiCollisionImpl() {
}

// Init and create graph
HIAI_StatusT HiCollisionImpl::HIAI_InitAndStartGraph(std::string graph_config) {
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
    int leaf_array[1] = {DEST_ENGINE_ID};  // leaf node id

    for (int i = 0; i < 1; i++) {
        hiai::EnginePortID target_port_config;
        target_port_config.graph_id = GRAPH_ID;
        target_port_config.engine_id = leaf_array[i];
        target_port_config.port_id = 0;
        graph->SetDataRecvFunctor(target_port_config, std::shared_ptr<HiCollisionImpl>(this));
    }
    return HIAI_OK;
}

/**
* @ingroup HiCollisionImpl
* @brief RecvData RecvData
* @param [in]
*/
HIAI_StatusT HiCollisionImpl::RecvData(const std::shared_ptr<void>& message) {
    std::shared_ptr<CollisionResultT> data = std::static_pointer_cast<CollisionResultT>(message);
    std::vector<CollisionData> result;
    float org_width = 1.0f;
    float org_height = 1.0f;
    if (0 != data->width && 0 != data->height && 0.0f != data->scale_width &&
        0.0f != data->scale_height) {
        org_width = data->width / data->scale_width;
        org_height = data->height / data->scale_height;
    } else {
        LOGE("invalid output data width,height[%d,%d], scale[%.5f,%.5f]", data->width,
             data->height, data->scale_width, data->scale_height);
    }
    int64_t timestamp = data->timestamp;

    // LOGD("RecvData data[%.5f,%.5f,%.5f,%.5f]", data->data[1], data->data[1], data->data[2],
    // data->data[3]);
    CollisionData coll;
    int max_index = 0;
    float max_value = data->data[0];
    for (uint8_t i = 0; i < 2; i++) {
        float cur_val = data->data[i];
        if (cur_val > max_value) {
            max_value = data->data[i];
            max_index = i;
        }
        coll.status_map.insert(std::make_pair(static_cast<CollisionStatus>(i), cur_val));
    }
    coll.status = static_cast<CollisionStatus>(max_index);
    coll.timestamp = timestamp;
    result.push_back(coll);
    if (callback_func) {
        callback_func(result);
    } else {
        LOGE("call back func is null");
    }
    return HIAI_OK;
}

bool HiCollisionImpl::init(const std::string& config_path) {
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
    engine_id.engine_id = SRC_ENGINE_ID;
    engine_id.port_id = 0;
    if (nullptr != graph) {
        isInit_ = true;
    }
    return STATE_SUCCESS;
}

bool HiCollisionImpl::deInit() {
    LOGI("deInit");
    if (nullptr != graph) {
        hiai::Graph::DestroyGraph(GRAPH_ID);
    }
    return false;
}

void HiCollisionImpl::setCallbackFunction(ca_callback_t func) {
    callback_func = func;
}

bool HiCollisionImpl::predict(const HIImgData& frame) {
    if (!isInit()) {
        return STATE_INVALID_OPERATION;
    }
    // assert
    if (0 == frame.width() || 0 == frame.height() || nullptr == frame.data()) {
        LOGW("HiRFImpl invalid input buff[%d,%d,%p]!", frame.width(), frame.height(), frame.data());
        return false;
    }
#if 0
#if 1
    std::string file_path(
        "/home/HwHiAiUser/HIAI_PROJECTS/workspace_mind_studio/test_data/"
        "0704-1140-data2-118_nv12.bin");
    std::shared_ptr<RFInputT> src_data = std::make_shared<RFInputT>();
    src_data->file = file_path;
    src_data->format = IMAGE_TYPE_NV12;
    src_data->width = 1280;
    src_data->height = 720;
    src_data->size = 1382400;
#else
    std::string file_path(
        "/home/HwHiAiUser/HIAI_PROJECTS/workspace_mind_studio/test_data/0704-1140-data2-118.jpg");
    std::shared_ptr<RFInputT> src_data = std::make_shared<RFInputT>();
    src_data->file = file_path;
    src_data->format = IMAGE_TYPE_JPEG;
    src_data->width = 1280;
    src_data->height = 720;
    src_data->size = 82973;
#endif
    graph->SendData(engine_id, "RFInputT", std::static_pointer_cast<void>(src_data));
    LOGI("predict main src file:%s", src_data->file.c_str());
#else
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
// if(HIAI_QUEUE_FULL == hiai_ret)
// {
//     HIAI_ENGINE_LOG(HIAI_IDE_INFO, "[Mind_road_following_dataset] queue full, sleep 200ms");
//     usleep(SEND_DATA_INTERVAL_MS);
// }
// }while(hiai_ret == HIAI_QUEUE_FULL);

// if(HIAI_OK != hiai_ret){
//     HIAI_ENGINE_LOG(HIAI_IDE_ERROR, "[Mind_road_following_dataset] SendData batch %u failed!
//     error code: %u", batchId, hiai_ret);
// }
#endif
    return true;
}

HIAI_StatusT HiCollisionImpl::makeImageInfo(NewImageParaT* imgData, const HIImgData& input) {
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

bool HiCollisionImpl::isInit() const {
    return isInit_;
}

HiCollisionAvoidance::HiCollisionAvoidance() {
    impl = new HiCollisionImpl();
}

HiCollisionAvoidance::~HiCollisionAvoidance() {
    if (nullptr != impl) {
        delete (reinterpret_cast<HiCollisionImpl*>(impl));
        impl = nullptr;
    }
}

bool HiCollisionAvoidance::init(const std::string& config_path) {
    if (nullptr != impl) {
        return (reinterpret_cast<HiCollisionImpl*>(impl))->init(config_path);
    } else {
        LOGE("HiCollisionAvoidance::init can not init!");
        return false;
    }
}

bool HiCollisionAvoidance::deInit() {
    if (nullptr != impl && isInit()) {
        return (reinterpret_cast<HiCollisionImpl*>(impl))->deInit();
    } else {
        LOGE("HiCollisionAvoidance::deInit can not init!");
        return false;
    }
}

void HiCollisionAvoidance::setCallbackFunction(ca_callback_t func) {
    if (nullptr != impl && isInit()) {
        return (reinterpret_cast<HiCollisionImpl*>(impl))->setCallbackFunction(func);
    } else {
        LOGE("HiCollisionAvoidance::setCallbackFunction can not init!");
        return;
    }
}

bool HiCollisionAvoidance::predict(const HIImgData& frame) {
    if (nullptr != impl && isInit()) {
        return (reinterpret_cast<HiCollisionImpl*>(impl))->predict(frame);
    } else {
        LOGE("HiCollisionAvoidance::predict can not init!");
        return false;
    }
}

bool HiCollisionAvoidance::isInit() const {
    if (nullptr != impl) {
        return (reinterpret_cast<HiCollisionImpl*>(impl))->isInit();
    } else {
        LOGE("HiCollisionAvoidance::isInit can not init!");
        return false;
    }
}

}  // namespace hi
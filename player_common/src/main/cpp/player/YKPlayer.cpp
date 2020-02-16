//
// Created by 阳坤 on 2020-01-19.
//

#include "include/YKPlayer.h"


//异步 函数指针 void* (*__start_routine)(void*) 准备工作 prepare
void *customTaskPrepareThread(void *pVoid) {
    YKPlayer *ykPlayer = static_cast<YKPlayer *>(pVoid);
    LOGD("customTashPrepareThread-->")
    ykPlayer->prepare_();
    return 0;//这里是坑一定要记得 return;

}

//异步 函数指针 开始播放工作 start
void *customTaskStartThread(void *pVoid) {
    YKPlayer *ykPlayer = static_cast<YKPlayer *>(pVoid);
    ykPlayer->start_();
    return 0;//这里是坑一定要记得 return;
}


YKPlayer::YKPlayer() {

}

YKPlayer::YKPlayer(const char *dataSource, JNICallback *pCallback) {
    // 这里有坑，这里赋值之后，不能给其他地方用，因为被释放了，变成了悬空指针
    // this->data_source = data_source;

    //解决上面的坑，自己 copy 才行 +1 在 C++ 中有一个 \n
    this->data_source = new char[strlen(dataSource) + 1];
    strcpy(this->data_source, dataSource);
    this->pCallback = pCallback;
}

YKPlayer::~YKPlayer() {
    if (this->data_source) {
        delete this->data_source;
        this->data_source = 0;
    }

}

void YKPlayer::prepare() {
    //解析流媒体，通过 ffmpeg  API 来解析 dataSource
    //这里需要异步，由于这个函数从 Java 主线程调用的，所以这里需要创建一个异步线程
    pthread_create(&pid_prepare, 0, customTaskPrepareThread, this);

}

/**
 * 该函数是真正的解封装，是在子线程开启并调用的。
 */
void YKPlayer::prepare_() {
    LOGD("第一步 打开流媒体地址");
    //1. 打开流媒体地址(文件路径、直播地址)
    // 可以初始为NULL，如果初始为NULL，当执行avformat_open_input函数时，内部会自动申请avformat_alloc_context，这里干脆手动申请
    // 封装了媒体流的格式信息
    formatContext = avformat_alloc_context();

    //字典: 键值对
    AVDictionary *dictionary = 0;
    av_dict_set(&dictionary, "timeout", "5000000", 0);//单位是微妙


    /**
     *
     * @param AVFormatContext: 传入一个 format 上下文是一个二级指针
     * @param const char *url: 播放源
     * @param ff_const59 AVInputFormat *fmt: 输入的封住格式，一般让 ffmpeg 自己去检测，所以给了一个 0
     * @param AVDictionary **options: 字典参数
     */
    int result = avformat_open_input(&formatContext, data_source, 0, &dictionary);
    //result -13--> 没有读写权限
    //result -99--> 第三个参数写 NULl
    LOGD("avformat_open_input-->    %d，%s", result, data_source);
    //释放字典
    av_dict_free(&dictionary);


    if (result) {//0 on success true
        // 你的文件路径，或，你的文件损坏了，需要告诉用户
        // 把错误信息，告诉给Java层去（回调给Java）
        if (pCallback) {
            pCallback->onErrorAction(THREAD_CHILD, FFMPEG_CAN_NOT_OPEN_URL);
        }
        return;
    }

    //第二步 查找媒体中的音视频流的信息
    LOGD("第二步 查找媒体中的音视频流的信息");
    result = avformat_find_stream_info(formatContext, 0);
    if (result < 0) {
        if (pCallback) {
            pCallback->onErrorAction(THREAD_CHILD, FFMPEG_CAN_NOT_FIND_STREAMS);
            return;
        }
    }
    //第三步 根据流信息，流的个数，循环查找，音频流 视频流
    LOGD("第三步 根据流信息，流的个数，循环查找，音频流 视频流");
    //nb_streams = 流的个数
    for (int stream_index = 0; stream_index < formatContext->nb_streams; ++stream_index) {
        //第四步 获取媒体流 音视频
        LOGD("第四步 获取媒体流 音视频");
        AVStream *stream = formatContext->streams[stream_index];


        //第五步 从 stream 流中获取解码这段流的参数信息，区分到底是 音频还是视频
        LOGD("第五步 从 stream 流中获取解码这段流的参数信息，区分到底是 音频还是视频");
        AVCodecParameters *codecParameters = stream->codecpar;

        //第六步 通过流的编解码参数中的编解码 ID ,来获取当前流的解码器
        LOGD("第六步 通过流的编解码参数中的编解码 ID ,来获取当前流的解码器");
        AVCodec *codec = avcodec_find_decoder(codecParameters->codec_id);
        //有可能不支持当前解码
        //找不到解码器，重新编译 ffmpeg --enable-librtmp
        if (!codec) {
            pCallback->onErrorAction(THREAD_CHILD, FFMPEG_FIND_DECODER_FAIL);
            return;
        }

        //第七步 通过拿到的解码器，获取解码器上下文
        LOGD("第七步 通过拿到的解码器，获取解码器上下文");
        AVCodecContext *codecContext = avcodec_alloc_context3(codec);
        if (!codecContext) {
            pCallback->onErrorAction(THREAD_CHILD, FFMPEG_ALLOC_CODEC_CONTEXT_FAIL);
            return;
        }

        //第八步 给解码器上下文 设置参数
        LOGD("第八步 给解码器上下文 设置参数");
        result = avcodec_parameters_to_context(codecContext, codecParameters);
        if (result < 0) {
            pCallback->onErrorAction(THREAD_CHILD, FFMPEG_CODEC_CONTEXT_PARAMETERS_FAIL);
            return;
        }

        //第九步 打开解码器
        LOGD("第九步 打开解码器");
        result = avcodec_open2(codecContext, codec, 0);
        if (result) {
            pCallback->onErrorAction(THREAD_CHILD, FFMPEG_OPEN_DECODER_FAIL);
            return;
        }

        //媒体流里面可以拿到时间基
        AVRational baseTime = stream->time_base;

        //第十步 从编码器参数中获取流类型 codec_type
        LOGD("第十步 从编码器参数中获取流类型 codec_type");
        if (codecParameters->codec_type == AVMEDIA_TYPE_AUDIO) {
            audioChannel = new AudioChannel(stream_index, codecContext,baseTime);
        } else if (codecParameters->codec_type == AVMEDIA_TYPE_VIDEO) {
            //获取视频帧 fps
            //平均帧率 == 时间基
            AVRational frame_rate = stream->avg_frame_rate;
            int fps_value = av_q2d(frame_rate);
            videoChannel = new VideoChannel(stream_index, codecContext, baseTime, fps_value);
            videoChannel->setRenderCallback(renderCallback);
        }
    }//end for

    //第十一步 如果流中没有音视频数据
    LOGD("第十一步 如果流中没有音视频数据");
    if (!audioChannel && !videoChannel) {
        pCallback->onErrorAction(THREAD_CHILD, FFMPEG_NOMEDIA);
        return;
    }

    //第十二步 要么有音频 要么有视频 要么音视频都有
    LOGD("第十二步 要么有音频 要么有视频 要么音视频都有");
    // 准备完毕，通知Android上层开始播放
    if (this->pCallback) {
        pCallback->onPrepared(THREAD_CHILD);
    }
}

/**
 * 开始播放
 */
void YKPlayer::start() {
    //声明是否播放的标志
    isPlaying = 1;
    if (videoChannel) {
        videoChannel->setAudioChannel(audioChannel);
        videoChannel->start();
    }
    if (audioChannel) {
        audioChannel->start();
    }

    //定义一个播放线程
    pthread_create(&pid_start, 0, customTaskStartThread, this);

}

/**
 * 读包 、未解码、音频/视频 包 放入队列
 */
void YKPlayer::start_() {
    // 循环 读音视频包
    while (isPlaying) {
        if (isStop) {
            av_usleep(2 * 1000);
            continue;
        }
        LOGD("start_");
        //内存泄漏点 1，解决方法 : 控制队列大小
        if (videoChannel && videoChannel->videoPackages.queueSize() > 100) {
            //休眠 等待队列中的数据被消费
            av_usleep(10 * 1000);
            continue;
        }

        //内存泄漏点 2 ，解决方案 控制队列大小
        if (audioChannel && audioChannel->audioPackages.queueSize() > 100) {
            //休眠 等待队列中的数据被消费
            av_usleep(10 * 1000);
            continue;
        }

        //AVPacket 可能是音频 可能是视频，没有解码的数据包
        AVPacket *packet = av_packet_alloc();
        //这一行执行完毕， packet 就有音视频数据了
        int ret = av_read_frame(formatContext, packet);
        /*       if (ret != 0) {
                   return;
               }*/
        if (!ret) {
            if (videoChannel && videoChannel->stream_index == packet->stream_index) {//视频包
                //未解码的 视频数据包 加入队列
                videoChannel->videoPackages.push(packet);
            } else if (audioChannel && audioChannel->stream_index == packet->stream_index) {//语音包
                //将语音包加入到队列中，以供解码使用
                audioChannel->audioPackages.push(packet);
            }
        } else if (ret == AVERROR_EOF) { //代表读取完毕了
            //TODO----
            LOGD("拆包完成 %s", "读取完成了")
            isPlaying = 0;
            stop();
            release();
            break;
        } else {
            LOGD("拆包 %s", "读取失败")
            break;//读取失败
        }
    }//end while
    //最后释放的工作
    isPlaying = 0;
    isStop = false;
    videoChannel->stop();
    audioChannel->stop();
}

void YKPlayer::setRenderCallback(RenderCallback renderCallback) {
    this->renderCallback = renderCallback;
}

void YKPlayer::stop() {
    //停止
    isStop = true;
    videoChannel->stop();
    audioChannel->stop();

}

void YKPlayer::release() {
    LOGD("YKPlayer ：%s","执行了销毁");
    isPlaying = false;
    stop();
    videoChannel->release();
    audioChannel->release();

}

void YKPlayer::restart() {
    isStop = false;
    videoChannel->restart();
    audioChannel->restart();
}

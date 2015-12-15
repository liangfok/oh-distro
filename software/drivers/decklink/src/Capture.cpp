/* -LICENSE-START-
** Copyright (c) 2013 Blackmagic Design
**
** Permission is hereby granted, free of charge, to any person or organization
** obtaining a copy of the software and accompanying documentation covered by
** this license (the "Software") to use, reproduce, display, distribute,
** execute, and transmit the Software, and to prepare derivative works of the
** Software, and to permit third-parties to whom the Software is furnished to
** do so, all subject to the following:
**
** The copyright notices in the Software and this entire statement, including
** the above license grant, this restriction and the following disclaimer,
** must be included in all copies of the Software, in whole or in part, and
** all derivative works of the Software, unless such copies or derivative
** works are solely in the form of machine-executable object code generated by
** a source language processor.
**
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
** IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
** FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
** SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
** FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
** ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
** DEALINGS IN THE SOFTWARE.
** -LICENSE-END-
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <csignal>

#include "DeckLinkAPI.h"
#include "Capture.h"
#include "Config.h"

#include <lcmtypes/bot_core/image_t.hpp>
#include <lcm/lcm-cpp.hpp>
#include <bot_core/timestamp.h>
#include <jpeg-utils/jpeg-utils.h>
#include <boost/thread/thread.hpp>
#include <boost/shared_ptr.hpp>
#include <queue>

static pthread_mutex_t	g_sleepMutex;
static pthread_cond_t	g_sleepCond;
static int				g_videoOutputFile = -1;
static int				g_audioOutputFile = -1;
static bool				g_do_exit = false;

IDeckLinkIterator*				g_deckLinkIterator = NULL;
IDeckLink*						g_deckLink = NULL;

IDeckLinkAttributes*			g_deckLinkAttributes = NULL;
bool							g_formatDetectionSupported;

IDeckLinkDisplayModeIterator*	g_displayModeIterator = NULL;
IDeckLinkDisplayMode*			g_displayMode = NULL;
char*							g_displayModeName = NULL;
BMDDisplayModeSupport			g_displayModeSupported;

DeckLinkCaptureDelegate*		g_delegate = NULL;

static BMDConfig		g_config;

static IDeckLinkInput* g_deckLinkInput = NULL;

static IDeckLinkOutput* g_deckLinkOutput = NULL;

static lcm::LCM* g_lcm = NULL;

static IDeckLinkVideoConversion* g_conversionInst = NULL;

static unsigned long g_frameCount = 0;

DeckLinkCaptureDelegate::DeckLinkCaptureDelegate() : m_refCount(0)
{
}

DeckLinkCaptureDelegate::~DeckLinkCaptureDelegate()
{
}

ULONG DeckLinkCaptureDelegate::AddRef(void)
{
    return __sync_add_and_fetch(&m_refCount, 1);
}

ULONG DeckLinkCaptureDelegate::Release(void)
{
    int32_t newRefValue = __sync_sub_and_fetch(&m_refCount, 1);
    if (newRefValue == 0)
    {
        delete this;
        return 0;
    }
    return newRefValue;
}


//----------------------------------------------------------------------------
namespace
{

template<typename T>
class SynchronizedQueue
{
public:

    SynchronizedQueue () :
        queue_(), mutex_(), cond_(), request_to_end_(false), enqueue_data_(true) { }

    void
    enqueue (const T& data)
    {
        boost::unique_lock<boost::mutex> lock (mutex_);

        if (enqueue_data_)
        {
            queue_.push (data);
            cond_.notify_one ();
        }
    }

    bool
    dequeue (T& result)
    {
        boost::unique_lock<boost::mutex> lock (mutex_);

        while (queue_.empty () && (!request_to_end_))
        {
            cond_.wait (lock);
        }

        if (request_to_end_)
        {
            doEndActions ();
            return false;
        }

        result = queue_.front ();
        queue_.pop ();

        return true;
    }

    void
    stopQueue ()
    {
        boost::unique_lock<boost::mutex> lock (mutex_);
        request_to_end_ = true;
        cond_.notify_one ();
    }

    unsigned int
    size ()
    {
        boost::unique_lock<boost::mutex> lock (mutex_);
        return static_cast<unsigned int> (queue_.size ());
    }

    bool
    isEmpty () const
    {
        boost::unique_lock<boost::mutex> lock (mutex_);
        return (queue_.empty ());
    }

private:
    void
    doEndActions ()
    {
        enqueue_data_ = false;

        while (!queue_.empty ())
        {
            queue_.pop ();
        }
    }

    std::queue<T> queue_;              // Use STL queue to store data
    mutable boost::mutex mutex_;       // The mutex to synchronise on
    boost::condition_variable cond_;   // The condition to wait for

    bool request_to_end_;
    bool enqueue_data_;
};

void convert(IDeckLinkMutableVideoFrame* videoFrame, bot_core::image_t& oImage) {

    oImage.width = videoFrame->GetWidth();
    oImage.height = videoFrame->GetHeight();
    oImage.row_stride = videoFrame->GetRowBytes();
    oImage.size = oImage.height*oImage.row_stride;
    oImage.nmetadata = 0;

    //printf("frame: %d x %d  (%d row bytes)\n", oImage.width, oImage.height, oImage.row_stride);

    // compress if necessary
    uint8_t* data = 0;
    videoFrame->GetBytes((void**)&data);


    bool shouldCompress = true;

    if (shouldCompress) {

        //double oldMB = oImage.size / (1024.0*1024.0);

        int compressionQuality = g_config.m_compressionQuality;
        std::vector<uint8_t> dest(oImage.size);
        jpeg_compress_8u_bgra(data, oImage.width, oImage.height, oImage.row_stride,
                              dest.data(), &oImage.size, compressionQuality);
        oImage.pixelformat = bot_core::image_t::PIXEL_FORMAT_MJPEG;
        oImage.data.resize(oImage.size);
        std::copy(dest.data(), dest.data()+oImage.size, oImage.data.begin());

        //double newMB = oImage.size / (1024.0*1024.0);
        //printf("compression %.2f --> %.2f MB\n", oldMB, newMB);
    }

    // otherwise just set raw bytes
    else {

        oImage.pixelformat = bot_core::image_t::PIXEL_FORMAT_BGRA;
        oImage.data.resize(oImage.size);
        std::copy(data, data + oImage.size, oImage.data.begin());
    }
}

class FrameData
{
public:
    FrameData()
    {
        timestamp = 0;
        outputFrame = NULL;
    }

    FrameData(IDeckLinkMutableVideoFrame* frame, int64_t t) : timestamp(t), outputFrame(frame)
    {
    }
    int64_t timestamp;
    IDeckLinkMutableVideoFrame* outputFrame;
};


class FrameConsumer
{
public:

    FrameConsumer()
    {
    }

    void Start()
    {
        this->Thread = boost::shared_ptr<boost::thread>(
                    new boost::thread(boost::bind(&FrameConsumer::ThreadLoop, this)));

        this->Thread2 = boost::shared_ptr<boost::thread>(
                    new boost::thread(boost::bind(&FrameConsumer::ThreadLoop, this)));
    }

    void Stop()
    {
        this->Queue.stopQueue();
    }

    void ThreadLoop()
    {
        FrameData frameData;

        while (this->Queue.dequeue(frameData))
        {
            bot_core::image_t msg;
            msg.utime = frameData.timestamp;

            double latencyWarningTime = 0.25;
            double latencyTime = (bot_timestamp_now() - frameData.timestamp)*1e-6;
            if (latencyTime > latencyWarningTime)
            {
                printf("%.3f seconds behind.  %u frames in queue\n", latencyTime, this->Queue.size());
            }
            convert(frameData.outputFrame, msg);
            g_lcm->publish(g_config.m_lcmChannelName, &msg);
            frameData.outputFrame->Release();
        }
    }

    SynchronizedQueue<FrameData> Queue;
    boost::shared_ptr<boost::thread> Thread;
    boost::shared_ptr<boost::thread> Thread2;
};


FrameConsumer frameConsumer;

}


HRESULT DeckLinkCaptureDelegate::VideoInputFrameArrived(IDeckLinkVideoInputFrame* videoFrame, IDeckLinkAudioInputPacket* audioFrame)
{
    IDeckLinkVideoFrame*				rightEyeFrame = NULL;
    IDeckLinkVideoFrame3DExtensions*	threeDExtensions = NULL;
    void*								frameBytes;
    void*								audioFrameBytes;

    // Handle Video Frame
    if (videoFrame)
    {
        // If 3D mode is enabled we retreive the 3D extensions interface which gives.
        // us access to the right eye frame by calling GetFrameForRightEye() .
        if ( (videoFrame->QueryInterface(IID_IDeckLinkVideoFrame3DExtensions, (void **) &threeDExtensions) != S_OK) ||
             (threeDExtensions->GetFrameForRightEye(&rightEyeFrame) != S_OK))
        {
            rightEyeFrame = NULL;
        }

        if (threeDExtensions)
            threeDExtensions->Release();

        if (videoFrame->GetFlags() & bmdFrameHasNoInputSource)
        {
            printf("Frame received (#%lu) - No input signal detected\n", g_frameCount);
        }
        else
        {
            const char *timecodeString = NULL;
            if (g_config.m_timecodeFormat != 0)
            {
                IDeckLinkTimecode *timecode;
                if (videoFrame->GetTimecode(g_config.m_timecodeFormat, &timecode) == S_OK)
                {
                    timecode->GetString(&timecodeString);
                }
            }

            int64_t timestampNow = bot_timestamp_now();

            if (g_config.m_lcmChannelName)
            {
                IDeckLinkMutableVideoFrame* outputFrame;
                g_deckLinkOutput->CreateVideoFrame(videoFrame->GetWidth(), videoFrame->GetHeight(), videoFrame->GetWidth()*4, bmdFormat8BitBGRA, bmdFrameFlagDefault, &outputFrame);
                HRESULT convertResult = g_conversionInst->ConvertFrame(videoFrame, outputFrame);

                frameConsumer.Queue.enqueue(FrameData(outputFrame, timestampNow));
            }

            static int64_t baseTime = timestampNow;
            static uint64_t frameCount = g_frameCount;
            double elapsedTime = (timestampNow - baseTime) * 1e-6;
            if (elapsedTime > 1.0)
            {
                printf("capturing at %.2f fps.\n", (g_frameCount - frameCount)/elapsedTime);
                baseTime = timestampNow;
                frameCount = g_frameCount;
            }

            if (timecodeString)
                free((void*)timecodeString);

            if (g_videoOutputFile != -1)
            {
                videoFrame->GetBytes(&frameBytes);
                write(g_videoOutputFile, frameBytes, videoFrame->GetRowBytes() * videoFrame->GetHeight());

                if (rightEyeFrame)
                {
                    rightEyeFrame->GetBytes(&frameBytes);
                    write(g_videoOutputFile, frameBytes, videoFrame->GetRowBytes() * videoFrame->GetHeight());
                }
            }
        }

        if (rightEyeFrame)
            rightEyeFrame->Release();

        g_frameCount++;
    }

    // Handle Audio Frame
    if (audioFrame)
    {
        if (g_audioOutputFile != -1)
        {
            audioFrame->GetBytes(&audioFrameBytes);
            write(g_audioOutputFile, audioFrameBytes, audioFrame->GetSampleFrameCount() * g_config.m_audioChannels * (g_config.m_audioSampleDepth / 8));
        }
    }

    if (g_config.m_maxFrames > 0 && videoFrame && g_frameCount >= g_config.m_maxFrames)
    {
        g_do_exit = true;
        pthread_cond_signal(&g_sleepCond);
    }

    return S_OK;
}

HRESULT DeckLinkCaptureDelegate::VideoInputFormatChanged(BMDVideoInputFormatChangedEvents events, IDeckLinkDisplayMode *mode, BMDDetectedVideoInputFormatFlags)
{
    // This only gets called if bmdVideoInputEnableFormatDetection was set
    // when enabling video input
    HRESULT	result;
    char*	displayModeName = NULL;

    if (!(events & bmdVideoInputDisplayModeChanged))
        return S_OK;

    mode->GetName((const char**)&displayModeName);
    printf("Video format changed to %s\n", displayModeName);

    if (displayModeName)
        free(displayModeName);

    if (g_deckLinkInput)
    {
        g_deckLinkInput->StopStreams();

        result = g_deckLinkInput->EnableVideoInput(mode->GetDisplayMode(), g_config.m_pixelFormat, g_config.m_inputFlags);
        if (result != S_OK)
        {
            fprintf(stderr, "Failed to switch video mode\n");
            return S_OK;
        }

        g_deckLinkInput->StartStreams();
    }

    return S_OK;
}

static void sigfunc(int signum)
{
    if (signum == SIGINT || signum == SIGTERM)
        g_do_exit = true;

    pthread_cond_signal(&g_sleepCond);
}

void cleanup()
{
    delete g_lcm;

    if (g_videoOutputFile != 0)
        close(g_videoOutputFile);

    if (g_audioOutputFile != 0)
        close(g_audioOutputFile);

    if (g_displayModeName != NULL)
        free(g_displayModeName);

    if (g_displayMode != NULL)
        g_displayMode->Release();

    if (g_displayModeIterator != NULL)
        g_displayModeIterator->Release();

    if (g_conversionInst != NULL)
        g_conversionInst->Release();

    if (g_deckLinkInput != NULL)
    {
        g_deckLinkInput->Release();
        g_deckLinkInput = NULL;
    }

    if (g_deckLinkOutput != NULL)
    {
        g_deckLinkOutput->Release();
        g_deckLinkOutput = NULL;
    }

    if (g_deckLinkAttributes != NULL)
        g_deckLinkAttributes->Release();

    if (g_deckLink != NULL)
        g_deckLink->Release();

    if (g_deckLinkIterator != NULL)
        g_deckLinkIterator->Release();
}

int main(int argc, char *argv[])
{
    HRESULT							result;
    int								exitStatus = 1;
    int								idx;

    pthread_mutex_init(&g_sleepMutex, NULL);
    pthread_cond_init(&g_sleepCond, NULL);

    signal(SIGINT, sigfunc);
    signal(SIGTERM, sigfunc);
    signal(SIGHUP, sigfunc);

    // Process the command line arguments
    if (!g_config.ParseArguments(argc, argv))
    {
        g_config.DisplayUsage(exitStatus);
        cleanup();
        return exitStatus;
    }

    g_lcm = new lcm::LCM();
    g_conversionInst = CreateVideoConversionInstance();

    // Get the DeckLink device
    g_deckLinkIterator = CreateDeckLinkIteratorInstance();
    if (!g_deckLinkIterator)
    {
        fprintf(stderr, "This application requires the DeckLink drivers installed.\n");
        cleanup();
        return exitStatus;
    }

    idx = g_config.m_deckLinkIndex;

    while ((result = g_deckLinkIterator->Next(&g_deckLink)) == S_OK)
    {
        if (idx == 0)
            break;
        --idx;

        g_deckLink->Release();
    }

    if (result != S_OK || g_deckLink == NULL)
    {
        fprintf(stderr, "Unable to get DeckLink device %u\n", g_config.m_deckLinkIndex);
        cleanup();
        return exitStatus;
    }

    // Get the input (capture) interface of the DeckLink device
    result = g_deckLink->QueryInterface(IID_IDeckLinkInput, (void**)&g_deckLinkInput);
    if (result != S_OK)
    {
        cleanup();
        return exitStatus;
    }



    // Get the output (display) interface of the DeckLink device
    if (g_deckLink->QueryInterface(IID_IDeckLinkOutput, (void**)&g_deckLinkOutput) != S_OK)
    {
        cleanup();
        return exitStatus;
    }


    // Get the display mode
    if (g_config.m_displayModeIndex == -1)
    {
        // Check the card supports format detection
        result = g_deckLink->QueryInterface(IID_IDeckLinkAttributes, (void**)&g_deckLinkAttributes);
        if (result == S_OK)
        {
            result = g_deckLinkAttributes->GetFlag(BMDDeckLinkSupportsInputFormatDetection, &g_formatDetectionSupported);
            if (result != S_OK || !g_formatDetectionSupported)
            {
                fprintf(stderr, "Format detection is not supported on this device\n");
                cleanup();
                return exitStatus;
            }
        }

        g_config.m_inputFlags |= bmdVideoInputEnableFormatDetection;

        // Format detection still needs a valid mode to start with
        idx = 0;
    }
    else
    {
        idx = g_config.m_displayModeIndex;
    }

    result = g_deckLinkInput->GetDisplayModeIterator(&g_displayModeIterator);
    if (result != S_OK)
    {
        cleanup();
        return exitStatus;
    }

    while ((result = g_displayModeIterator->Next(&g_displayMode)) == S_OK)
    {
        if (idx == 0)
            break;
        --idx;

        g_displayMode->Release();
    }

    if (result != S_OK || g_displayMode == NULL)
    {
        fprintf(stderr, "Unable to get display mode %d\n", g_config.m_displayModeIndex);
        cleanup();
        return exitStatus;
    }

    // Get display mode name
    result = g_displayMode->GetName((const char**)&g_displayModeName);
    if (result != S_OK)
    {
        g_displayModeName = (char *)malloc(32);
        snprintf(g_displayModeName, 32, "[index %d]", g_config.m_displayModeIndex);
    }

    // Check display mode is supported with given options
    result = g_deckLinkInput->DoesSupportVideoMode(g_displayMode->GetDisplayMode(), g_config.m_pixelFormat, bmdVideoInputFlagDefault, &g_displayModeSupported, NULL);
    if (result != S_OK)
    {
        cleanup();
        return exitStatus;
    }

    if (g_displayModeSupported == bmdDisplayModeNotSupported)
    {
        fprintf(stderr, "The display mode %s is not supported with the selected pixel format\n", g_displayModeName);
        cleanup();
        return exitStatus;
    }

    if (g_config.m_inputFlags & bmdVideoInputDualStream3D)
    {
        if (!(g_displayMode->GetFlags() & bmdDisplayModeSupports3D))
        {
            fprintf(stderr, "The display mode %s is not supported with 3D\n", g_displayModeName);
            cleanup();
            return exitStatus;
        }
    }

    // Print the selected configuration
    g_config.DisplayConfiguration();

    // Configure the capture callback
    g_delegate = new DeckLinkCaptureDelegate();
    g_deckLinkInput->SetCallback(g_delegate);

    // Open output files
    if (g_config.m_videoOutputFile != NULL)
    {
        g_videoOutputFile = open(g_config.m_videoOutputFile, O_WRONLY|O_CREAT|O_TRUNC, 0664);
        if (g_videoOutputFile < 0)
        {
            fprintf(stderr, "Could not open video output file \"%s\"\n", g_config.m_videoOutputFile);
            cleanup();
            return exitStatus;
        }
    }

    if (g_config.m_audioOutputFile != NULL)
    {
        g_audioOutputFile = open(g_config.m_audioOutputFile, O_WRONLY|O_CREAT|O_TRUNC, 0664);
        if (g_audioOutputFile < 0)
        {
            fprintf(stderr, "Could not open audio output file \"%s\"\n", g_config.m_audioOutputFile);
            cleanup();
            return exitStatus;
        }
    }

    // Block main thread until signal occurs
    while (!g_do_exit)
    {
        // Start capturing
        result = g_deckLinkInput->EnableVideoInput(g_displayMode->GetDisplayMode(), g_config.m_pixelFormat, g_config.m_inputFlags);
        if (result != S_OK)
        {
            fprintf(stderr, "Failed to enable video input. Is another application using the card?\n");
        }
        else
        {
            result = g_deckLinkInput->EnableAudioInput(bmdAudioSampleRate48kHz, g_config.m_audioSampleDepth, g_config.m_audioChannels);
            if (result == S_OK)
            {
                frameConsumer.Start();

                result = g_deckLinkInput->StartStreams();
                if (result == S_OK)
                {
                    // All Okay.
                    exitStatus = 0;

                    pthread_mutex_lock(&g_sleepMutex);
                    pthread_cond_wait(&g_sleepCond, &g_sleepMutex);
                    pthread_mutex_unlock(&g_sleepMutex);

                    fprintf(stderr, "Stopping Capture\n");
                    frameConsumer.Stop();
                    g_deckLinkInput->StopStreams();
                    g_deckLinkInput->DisableAudioInput();
                    g_deckLinkInput->DisableVideoInput();
                }
            }
        }
    }

    cleanup();

    return exitStatus;
}

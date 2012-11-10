/*
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2012, Willow Garage, Inc.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of Willow Garage, Inc. nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include<opencv2/highgui/highgui.hpp>
#include<opencv2/imgproc/imgproc.hpp>

#include <opencv_creative/reader.h>

#include <DepthSense.hxx>

namespace creative
{
  void
  getAvailableNodes(DepthSense::Context context, DepthSense::ColorNode &color_node, DepthSense::DepthNode &depth_node)
  {
    // obtain the list of devices attached to the host
    std::vector<DepthSense::Device> devices = context.getDevices();
    for (std::vector<DepthSense::Device>::const_iterator iter = devices.begin(); iter != devices.end(); iter++)
    {
      DepthSense::Device device = *iter;
      // obtain the list of nodes of the current device
      std::vector<DepthSense::Node> nodes = device.getNodes();
      for (std::vector<DepthSense::Node>::const_iterator nodeIter = nodes.begin(); nodeIter != nodes.end(); nodeIter++)
      {
        DepthSense::Node node = *nodeIter;
        if (!color_node.isSet())
          color_node = node.as<DepthSense::ColorNode>();
        if (!depth_node.isSet())
          depth_node = node.as<DepthSense::DepthNode>();
      }
      break;
    }
    // return an unset color node
    if (!color_node.isSet())
      color_node = DepthSense::ColorNode();
    if (!depth_node.isSet())
      depth_node = DepthSense::DepthNode();
  }

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

  /** Class where all the magic happens to interace with the Creative camera
   * @return
   */
  class ReaderImpl
  {
  public:
    ReaderImpl()
    {
    }

    ~ReaderImpl()
    {
      context_.quit();
    }

    static void
    onNewColorSample(DepthSense::ColorNode obj, DepthSense::ColorNode::NewSampleReceivedData data)
    {
      // Read the color buffer and display
      int32_t w, h;
      DepthSense::FrameFormat_toResolution(data.captureConfiguration.frameFormat, &w, &h);

#if 1
      cv::Mat color_yuy2(h, w, CV_8UC2, const_cast<void*>((const void*) (data.colorMap)));
      {
        boost::mutex::scoped_lock lock(mutex_);
        cv::cvtColor(color_yuy2, color_, CV_YUV2RGB_YUY2);
      }
#else
      cv::Mat color_bgr(480, 640, CV_8UC3, const_cast<void*>((const void*) (data.colorMap)));
      cv::cvtColor(color_bgr, color_, CV_BGR2RGB);
#endif

      static size_t index = 0;
      // VERY HACKISH: but wait for another N frames (totally arbitrary) before getting a depth node in there
      if (index == 10)
      {
        // Get depth data
        context_.requestControl(depth_node_);
        depth_node_.setEnableDepthMap(true);
        DepthSense::DepthNode::DepthNode::Configuration depth_configuration(
            DepthSense::FRAME_FORMAT_QVGA, 30, DepthSense::DepthNode::CAMERA_MODE_CLOSE_MODE, true);
        depth_node_.setConfiguration(depth_configuration);
        depth_node_.setConfidenceThreshold(50);
        context_.releaseControl(depth_node_);

        context_.registerNode(depth_node_);
        ++index;
      }
      else
        ++index;
    }

    static void
    onNewDepthSample(DepthSense::DepthNode obj, DepthSense::DepthNode::NewSampleReceivedData data)
    {
      // Read the color buffer and display
      int32_t w, h;
      DepthSense::FrameFormat_toResolution(data.captureConfiguration.frameFormat, &w, &h);
      cv::Mat depth_single(h, w, CV_16UC1, const_cast<void*>((const void*) (data.depthMap)));
      {
        boost::mutex::scoped_lock lock(mutex_);
        depth_single.copyTo(depth_);
      }
    }

    static void
    initialize()
    {
      // create a connection to the DepthSense server at localhost
      context_ = DepthSense::Context::create();
      // get the first available color sensor
      getAvailableNodes(context_, color_node_, depth_node_);

      // enable the capture of the color map
      // Get RGB data
      context_.requestControl(color_node_);
      color_node_.setEnableColorMap(true);
      DepthSense::ColorNode::Configuration color_configuration(DepthSense::FRAME_FORMAT_QVGA, 30,
                                                               DepthSense::POWER_LINE_FREQUENCY_50HZ,
                                                               DepthSense::COMPRESSION_TYPE_YUY2);
      color_node_.setConfiguration(color_configuration);
      context_.releaseControl(color_node_);

      // connect a callback to the newSampleReceived event of the color node
      color_node_.newSampleReceivedEvent().connect(ReaderImpl::onNewColorSample);
      depth_node_.newSampleReceivedEvent().connect(ReaderImpl::onNewDepthSample);

      // Do not connect depth yet as that crashes the driver ...
      context_.registerNode(color_node_);
      context_.startNodes();

      // Spawn the thread that will just run
      thread_ = boost::thread(run);
    }

    void
    getImages(cv::Mat&color, cv::Mat& depth) const
    {
      {
        boost::mutex::scoped_lock lock(mutex_);
        color_.copyTo(color);
        depth_.copyTo(depth);
      }
    }

    static bool is_initialized_;

  private:
    static void
    run()
    {
      context_.run();
    }

    static DepthSense::Context context_;
    static DepthSense::ColorNode color_node_;
    static DepthSense::DepthNode depth_node_;
    static boost::thread thread_;
    static cv::Mat color_;
    static cv::Mat depth_;
    static boost::mutex mutex_;
  };

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

  ReaderImpl *Reader::impl_ = new ReaderImpl();
  bool ReaderImpl::is_initialized_ = false;
  DepthSense::Context ReaderImpl::context_;
  boost::thread ReaderImpl::thread_;
  DepthSense::ColorNode ReaderImpl::color_node_ = DepthSense::ColorNode();
  DepthSense::DepthNode ReaderImpl::depth_node_ = DepthSense::DepthNode();
  cv::Mat ReaderImpl::color_;
  cv::Mat ReaderImpl::depth_;
  boost::mutex ReaderImpl::mutex_;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

  /** Clean all the contexts and unregister the nodes
   */
  Reader::~Reader()
  {
    delete impl_;
  }

  void
  Reader::initialize()
  {
    impl_->initialize();
  }

  bool
  Reader::isInitialized()
  {
    return impl_->is_initialized_;
  }

  void
  Reader::getImages(cv::Mat&color, cv::Mat& depth)
  {
    impl_->getImages(color, depth);
  }
}

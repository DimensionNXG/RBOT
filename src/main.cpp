/**
 *   #, #,         CCCCCC  VV    VV MM      MM RRRRRRR
 *  %  %(  #%%#   CC    CC VV    VV MMM    MMM RR    RR
 *  %    %## #    CC        V    V  MM M  M MM RR    RR
 *   ,%      %    CC        VV  VV  MM  MM  MM RRRRRR
 *   (%      %,   CC    CC   VVVV   MM      MM RR   RR
 *     #%    %*    CCCCCC     VV    MM      MM RR    RR
 *    .%    %/
 *       (%.      Computer Vision & Mixed Reality Group
 *                For more information see <http://cvmr.info>
 *
 * This file is part of RBOT.
 *
 *  @copyright:   RheinMain University of Applied Sciences
 *                Wiesbaden Rüsselsheim
 *                Germany
 *     @author:   Henning Tjaden
 *                <henning dot tjaden at gmail dot com>
 *    @version:   1.0
 *       @date:   30.08.2018
 *
 * RBOT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * RBOT is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with RBOT. If not, see <http://www.gnu.org/licenses/>.
 */

#include <QApplication>
#include <QThread>
#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>

#include "object3d.h"
#include "pose_estimator6d.h"

#include <xslam/xslam_sdk.hpp>
#include <xslam/xslam_geometry.hpp>
#include <xslam/xslam_io.hpp>
#include <future>

using namespace std;
using namespace cv;

cv::Mat drawResultOverlay(const vector<Object3D*>& objects, const cv::Mat& frame)
{
    // render the models with phong shading
    RenderingEngine::Instance()->setLevel(0);
    
    vector<Point3f> colors;
    colors.push_back(Point3f(1.0, 0.5, 0.0));
    //colors.push_back(Point3f(0.2, 0.3, 1.0));
    RenderingEngine::Instance()->renderShaded(vector<Model*>(objects.begin(), objects.end()), GL_FILL, colors, true);
    
    // download the rendering to the CPU
    Mat rendering = RenderingEngine::Instance()->downloadFrame(RenderingEngine::RGB);
    
    // download the depth buffer to the CPU
    Mat depth = RenderingEngine::Instance()->downloadFrame(RenderingEngine::DEPTH);
    
    // compose the rendering with the current camera image for demo purposes (can be done more efficiently directly in OpenGL)
    Mat result = frame.clone();
    for(int y = 0; y < frame.rows; y++)
    {
        for(int x = 0; x < frame.cols; x++)
        {
            Vec3b color = rendering.at<Vec3b>(y,x);
            if(depth.at<float>(y,x) != 0.0f)
            {
                result.at<Vec3b>(y,x)[0] = color[2];
                result.at<Vec3b>(y,x)[1] = color[1];
                result.at<Vec3b>(y,x)[2] = color[0];
            }
        }
    }
    return result;
}

static cv::Mat s_grabbed_rgb_unprocessed;
static cv::Mat s_rgb;
static bool s_run = false;
static std::future<void> s_future;

void onRGB( xslam_rgb *rgb){
    s_grabbed_rgb_unprocessed = cv::Mat( static_cast<int>(1.5*rgb->height), rgb->width, CV_8UC1, rgb->data );
    cv::cvtColor( s_grabbed_rgb_unprocessed, s_rgb, cv::COLOR_YUV2BGR_I420 );
}

void try_start_camera(){
    while( !xslam_camera_is_detected() && s_run ){
        std::this_thread::sleep_for( std::chrono::milliseconds(2000) );
    }
    if( s_run ){
        xslam_start_camera();
    }
}

void xslam_stop_capture(){
    s_run = false;
    xslam_stop();
    return;
}

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    // camera image size
    int width = 640;
    int height = 480;
    
    // near and far plane of the OpenGL view frustum
    float zNear = 10.0;
    float zFar = 10000.0;
    
    // camera instrinsics
    //    Matx33f K = Matx33f(650.048, 0, 324.328, 0, 647.183, 257.323, 0, 0, 1);
    //    Matx14f distCoeffs =  Matx14f(0.0, 0.0, 0.0, 0.0);
    //xVisio intrinsics
    Matx33f K = Matx33f(442.115, 0, 318.838, 0, 442.115, 119.899, 0, 0, 1);
    //    k1 = 0.119386,
    //    k2 = -0.334424,
    //    p1 = -0.00100931,
    //    p2 = -0.00077402,
    //    k3 = 0.219367
    float distortion_coeff_data[5] = {0.119386,
                                      -0.334424,
                                      -0.00100931,
                                      -0.00077402,
                                      0.219367 };
    Mat distCoeffs =  Mat(5,1,CV_32FC1, distortion_coeff_data);
    
    // distances for the pose detection template generation
    vector<float> distances = {200.0f, 400.0f, 600.0f};
    
    // load 3D objects
    vector<Object3D*> objects;
    objects.push_back(new Object3D("data/squirrel_demo_low.obj", 15, -35, 515, 55, -20, 205, 1.0, 0.55f, distances));
    //objects.push_back(new Object3D("data/a_second_model.obj", -50, 0, 600, 30, 0, 180, 1.0, 0.55f, distances2));
    
    // create the pose estimator
    PoseEstimator6D* poseEstimator = new PoseEstimator6D(width, height, zNear, zFar, K, distCoeffs, objects);
    
    // move the OpenGL context for offscreen rendering to the current thread, if run in a seperate QT worker thread (unnessary in this example)
    //RenderingEngine::Instance()->getContext()->moveToThread(this);
    
    // active the OpenGL context for the offscreen rendering engine during pose estimation
    RenderingEngine::Instance()->makeCurrent();
    
    int timeout = 0;
    
    bool showHelp = true;
    
    //set xVisio parameters and start grabbing rgb frames
    if(xslam_has_rgb()){
        xslam_set_rgb_resolution( xslam_rgb_resolution::RGB_640x480 );
        xslam_rgb_callback( onRGB );
    }
    s_run = true;
    s_future = std::async( std::launch::async, try_start_camera );

    Mat frame;
    int key =-1;

    while(true)
    {
        if(s_rgb.empty())
            continue;

        // obtain an input image
        s_rgb.copyTo(frame);
        std::cout << "new frame"<<std::endl;

        // If the frame is empty, break immediately
        if (frame.empty())
            break;


        // the main pose uodate call
        poseEstimator->estimatePoses(frame, false, true);
        
        cout << objects[0]->getPose() << endl;
        
        // render the models with the resulting pose estimates ontop of the input image
        Mat result = drawResultOverlay(objects, frame);
        
        if(showHelp)
        {
            putText(result, "Press '1' to initialize", Point(150, 250), FONT_HERSHEY_DUPLEX, 1.0, Scalar(255, 255, 255), 1);
            putText(result, "or 'c' to quit", Point(205, 285), FONT_HERSHEY_DUPLEX, 1.0, Scalar(255, 255, 255), 1);
        }
        
        imshow("result", result);
        
        int key = waitKey(timeout);
        
        // start/stop tracking the first object
        if(key == (int)'1')
        {
            poseEstimator->toggleTracking(frame, 0, false);
            poseEstimator->estimatePoses(frame, true, false);
            timeout = 1;
            showHelp = !showHelp;
        }
        if(key == (int)'2') // the same for a second object
        {
            //poseEstimator->toggleTracking(frame, 1, false);
            //poseEstimator->estimatePoses(frame, false, false);
        }
        // reset the system to the initial state
        if(key == (int)'r')
            poseEstimator->reset();
        // stop the demo
        if(key == (int)'c')
            break;
    }
    
    // deactivate the offscreen rendering OpenGL context
    RenderingEngine::Instance()->doneCurrent();
    
    // clean up
    RenderingEngine::Instance()->destroy();

    xslam_stop_capture();
    
    for(int i = 0; i < objects.size(); i++)
    {
        delete objects[i];
    }
    objects.clear();
    
    delete poseEstimator;
}

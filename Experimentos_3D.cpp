/*
 * This file is part of the OpenKinect Project. http://www.openkinect.org
 *
 * Copyright (c) 2011 individual OpenKinect contributors. See the CONTRIB file
 * for details.
 *
 * This code is licensed to you under the terms of the Apache License, version
 * 2.0, or, at your option, the terms of the GNU General Public License,
 * version 2.0. See the APACHE20 and GPL2 files for the text of the licenses,
 * or the following URLs:
 * http://www.apache.org/licenses/LICENSE-2.0
 * http://www.gnu.org/licenses/gpl-2.0.txt
 *
 * If you redistribute this file in source form, modified or unmodified, you
 * may:
 *   1) Leave this header intact and distribute it under the same terms,
 *      accompanying it with the APACHE20 and GPL20 files, or
 *   2) Delete the Apache 2.0 clause and accompany it with the GPL2 file, or
 *   3) Delete the GPL v2 clause and accompany it with the APACHE20 file
 * In all cases you must keep the copyright notice intact and include a copy
 * of the CONTRIB file.
 *
 * Binary distributions must follow the binary distribution requirements of
 * either License.
 */


#include <iostream>
#include <signal.h>

#include <opencv2/opencv.hpp>

#include <libfreenect2/libfreenect2.hpp>
#include <libfreenect2/frame_listener_impl.h>
#include <libfreenect2/threading.h>
#include <libfreenect2/registration.h>
#include <libfreenect2/packet_pipeline.h>

#include "opencv2/core/core.hpp"
#include "opencv2/face.hpp"
#include "opencv2/highgui/highgui.hpp"
#include "opencv2/imgproc/imgproc.hpp"
#include "opencv2/objdetect/objdetect.hpp"
#include <opencv2/video/tracking.hpp>

using namespace cv;
using namespace std;
using namespace cv::face;

// Image parameters
#define WIDTH 512             // Input image width
#define HEIGHT 424              // Input image height
#define SIZE 217088             // Input image size
// Calibration parameters
#define DEPTH_RANGE 2048          // Range of depth values [0,DEPTH_RANGE[
#define DEPTH_FX 5.8498272251689014e+02   // Scaling factor for the x axis
#define DEPTH_FY 5.8509835924680374e+02   // Scaling factor for the y axis
#define DEPTH_CX 3.1252165122981484e+02   // Camera center for the x axis
#define DEPTH_CY 2.3821622578866226e+02   // Camera center for the y axis
#define DEPTH_Z1 1.1863           // Disparity to mm - 1st parameter
#define DEPTH_Z2 2842.5           // Disparity to mm - 2nd parameter
#define DEPTH_Z3 123.6            // Disparity to mm - 3rd parameter
#define DEPTH_Z4 95.45454545        // Disparity to mm - 4th parameter (750*RESOLUTION)
#define DEPTH_LTHRESHOLD 400        // Minimum disparity value
#define DEPTH_CTHRESHOLD 675          // Maximum disparity value
#define DEPTH_THRESHOLD 875         // Maximum disparity value
// Detection parameters
#define X_WIDTH 1800.0            // Orthogonal projection width - in mm
#define Y_WIDTH 1600.0            // Orthogonal projection height - in mm
#define RESOLUTION 0.127272727        // Resolution in pixels per mm
#define FACE_SIZE 21            // Face size - 165*RESOLUTION
#define FACE_HALF_SIZE 10         // (165*RESOLUTION)/2
// Normalization parameters
#define MODEL_WIDTH 48.0
#define MODEL_HEIGHT_1 56.0
#define MODEL_HEIGHT_2 16.0
#define MODEL_RESOLUTION 1.0
#define MAX_DEPTH_VALUE 5000
#define OUTLIER_THRESHOLD 15.0
#define OUTLIER_SQUARED_THRESHOLD 225.0
#define MAX_ICP_ITERATIONS 200

const string PATH_CASCADE_FACE = "/home/matheusm/ALL_Spring2003_3D.xml";

bool protonect_shutdown = false;

void sigint_handler(int s)
{
  protonect_shutdown = true;
}

// Compute rotation matrix and its inverse matrix
void computeRotationMatrix(double matrix[3][3], double imatrix[3][3], double aX, double aY, double aZ) {
  double cosX, cosY, cosZ, sinX, sinY, sinZ, d;

  cosX = cos(aX);
  cosY = cos(aY);
  cosZ = cos(aZ);
  sinX = sin(aX);
  sinY = sin(aY);
  sinZ = sin(aZ);

  matrix[0][0] = cosZ*cosY+sinZ*sinX*sinY;
  matrix[0][1] = sinZ*cosY-cosZ*sinX*sinY;
  matrix[0][2] = cosX*sinY;
  matrix[1][0] = -sinZ*cosX;
  matrix[1][1] = cosZ*cosX;
  matrix[1][2] = sinX;
  matrix[2][0] = sinZ*sinX*cosY-cosZ*sinY;
  matrix[2][1] = -cosZ*sinX*cosY-sinZ*sinY;
  matrix[2][2] = cosX*cosY;

  d = matrix[0][0]*(matrix[2][2]*matrix[1][1]-matrix[2][1]*matrix[1][2])-matrix[1][0]*(matrix[2][2]*matrix[0][1]-matrix[2][1]*matrix[0][2])+matrix[2][0]*(matrix[1][2]*matrix[0][1]-matrix[1][1]*matrix[0][2]);

  imatrix[0][0] = (matrix[2][2]*matrix[1][1]-matrix[2][1]*matrix[1][2])/d;
  imatrix[0][1] = -(matrix[2][2]*matrix[0][1]-matrix[2][1]*matrix[0][2])/d;
  imatrix[0][2] = (matrix[1][2]*matrix[0][1]-matrix[1][1]*matrix[0][2])/d;
  imatrix[1][0] = -(matrix[2][2]*matrix[1][0]-matrix[2][0]*matrix[1][2])/d;
  imatrix[1][1] = (matrix[2][2]*matrix[0][0]-matrix[2][0]*matrix[0][2])/d;
  imatrix[1][2] = -(matrix[1][2]*matrix[0][0]-matrix[1][0]*matrix[0][2])/d;
  imatrix[2][0] = (matrix[2][1]*matrix[1][0]-matrix[2][0]*matrix[1][1])/d;
  imatrix[2][1] = -(matrix[2][1]*matrix[0][0]-matrix[2][0]*matrix[0][1])/d;
  imatrix[2][2] = (matrix[1][1]*matrix[0][0]-matrix[1][0]*matrix[0][1])/d;
}

void compute_projection(IplImage *p, IplImage *m, CvPoint3D64f *xyz, int n, double matrix[3][3], double background) {
  static int flag = 1, height, width, size, cx, cy, *li, *lj, *lc;
  int i, j, k, l, c, t;
  double d;

  if(flag) {
    flag = 0;

    height = p->height;
    width = p->width;
    size = height*width;

    li = (int *) malloc(3*size*sizeof(int));
    lj = li+size;
    lc = lj+size;

    cx = width/2;
    cy = height/2;
  }

  // Compute projection
  cvSet(p, cvRealScalar(-DBL_MAX), NULL);
  cvSet(m, cvRealScalar(0), NULL);
  for(i=0; i < n; i++) {
    j = cy-cvRound(xyz[i].x*matrix[1][0]+xyz[i].y*matrix[1][1]+xyz[i].z*matrix[1][2]);
    k = cx+cvRound(xyz[i].x*matrix[0][0]+xyz[i].y*matrix[0][1]+xyz[i].z*matrix[0][2]);
    d = xyz[i].x*matrix[2][0]+xyz[i].y*matrix[2][1]+xyz[i].z*matrix[2][2];

    if(j >= 0 && k >= 0 && j < height && k < width && d > CV_IMAGE_ELEM(p, double, j, k)) {
      CV_IMAGE_ELEM(p, double, j, k) = d;
      CV_IMAGE_ELEM(m, uchar, j, k) = 1;
    }
  }

  // Hole filling
  k=l=0;
  for(i=1; i < height-1; i++)
    for(j=1; j < width-1; j++)
      if(!CV_IMAGE_ELEM(m, uchar, i, j) && (CV_IMAGE_ELEM(m, uchar, i, j-1) || CV_IMAGE_ELEM(m, uchar, i, j+1) || CV_IMAGE_ELEM(m, uchar, i-1, j) || CV_IMAGE_ELEM(m, uchar, i+1, j))) {
        li[l] = i;
        lj[l] = j;
        lc[l] = 1;
        l++;
      }

  while(k < l) {
    i = li[k];
    j = lj[k];
    c = lc[k];
    if(!CV_IMAGE_ELEM(m, uchar, i, j) && i > 0 && i < height-1 && j > 0 && j < width-1 && c < FACE_HALF_SIZE) {
      CV_IMAGE_ELEM(m, uchar, i, j) = c+1;
      t = 0;
      d = 0.0f;
      if(CV_IMAGE_ELEM(m, uchar, i, j-1) && CV_IMAGE_ELEM(m, uchar, i, j-1) <= c) {
        t++;
        d += CV_IMAGE_ELEM(p, double, i, j-1);
      }
      else {
        li[l] = i;
        lj[l] = j-1;
        lc[l] = c+1;
        l++;
      }
      if(CV_IMAGE_ELEM(m, uchar, i, j+1) && CV_IMAGE_ELEM(m, uchar, i, j+1) <= c) {
        t++;
        d += CV_IMAGE_ELEM(p, double, i, j+1);
      }
      else {
        li[l] = i;
        lj[l] = j+1;
        lc[l] = c+1;
        l++;
      }
      if(CV_IMAGE_ELEM(m, uchar, i-1, j) && CV_IMAGE_ELEM(m, uchar, i-1, j) <= c) {
        t++;
        d += CV_IMAGE_ELEM(p, double, i-1, j);
      }
      else {
        li[l] = i-1;
        lj[l] = j;
        lc[l] = c+1;
        l++;
      }
      if(CV_IMAGE_ELEM(m, uchar, i+1, j) && CV_IMAGE_ELEM(m, uchar, i+1, j) <= c) {
        t++;
        d += CV_IMAGE_ELEM(p, double, i+1, j);
      }
      else {
        li[l] = i+1;
        lj[l] = j;
        lc[l] = c+1;
        l++;
      }
      CV_IMAGE_ELEM(p, double, i, j) = d/(double)t;
    }
    k++;
  }

  // Final adjustments
  for(i=0; i < height; i++)
    for(j=0; j < width; j++) {
      if(CV_IMAGE_ELEM(p, double, i, j) == -DBL_MAX)
        CV_IMAGE_ELEM(p, double, i, j) = background;
      if(CV_IMAGE_ELEM(m, uchar, i, j))
        CV_IMAGE_ELEM(m, uchar, i, j) = 1;
    }
}

int main(int argc, char *argv[])
{
  std::string program_path(argv[0]);
  size_t executable_name_idx = program_path.rfind("Protonect");

  std::string binpath = "/";

  if(executable_name_idx != std::string::npos)
  {
    binpath = program_path.substr(0, executable_name_idx);
  }

  libfreenect2::Freenect2 freenect2;
  libfreenect2::Freenect2Device *dev = 0;
  libfreenect2::PacketPipeline *pipeline = 0;

  if(freenect2.enumerateDevices() == 0)
  {
    std::cout << "no device connected!" << std::endl;
    return -1;
  }

  std::string serial = freenect2.getDefaultDeviceSerialNumber();

  for(int argI = 1; argI < argc; ++argI)
  {
    const std::string arg(argv[argI]);

    if(arg == "cpu")
    {
      if(!pipeline)
        pipeline = new libfreenect2::CpuPacketPipeline();
    }
    else if(arg == "gl")
    {
#ifdef LIBFREENECT2_WITH_OPENGL_SUPPORT
      if(!pipeline)
        pipeline = new libfreenect2::OpenGLPacketPipeline();
#else
      std::cout << "OpenGL pipeline is not supported!" << std::endl;
#endif
    }
    else if(arg == "cl")
    {
#ifdef LIBFREENECT2_WITH_OPENCL_SUPPORT
      if(!pipeline)
        pipeline = new libfreenect2::OpenCLPacketPipeline();
#else
      std::cout << "OpenCL pipeline is not supported!" << std::endl;
#endif
    }
    else if(arg.find_first_not_of("0123456789") == std::string::npos) //check if parameter could be a serial number
    {
      serial = arg;
    }
    else
    {
      std::cout << "Unknown argument: " << arg << std::endl;
    }
  }

  if(pipeline)
  {
    dev = freenect2.openDevice(serial, pipeline);
  }
  else
  {
    dev = freenect2.openDevice(serial);
  }

  if(dev == 0)
  {
    std::cout << "failure opening device!" << std::endl;
    return -1;
  }

  signal(SIGINT,sigint_handler);
  protonect_shutdown = false;

  libfreenect2::SyncMultiFrameListener listener(libfreenect2::Frame::Color | libfreenect2::Frame::Ir | libfreenect2::Frame::Depth);
  libfreenect2::FrameMap frames;
  //libfreenect2::Frame undistorted(512, 424, 4), registered(512, 424, 4);

  dev->setColorFrameListener(&listener);
  dev->setIrAndDepthFrameListener(&listener);
  dev->start();

  std::cout << "device serial: " << dev->getSerialNumber() << std::endl;
  std::cout << "device firmware: " << dev->getFirmwareVersion() << std::endl;

  libfreenect2::Registration* registration = new libfreenect2::Registration(dev->getIrCameraParams(), dev->getColorCameraParams());

  int width = 512;
  int height = 424;

  cv::Mat cv_img_cords = cv::Mat(1, width*height, CV_32FC2);
  Mat cv_img_corrected_cords;
  for (int r = 0; r < height; ++r) {
      for (int c = 0; c < width; ++c) {
          cv_img_cords.at<cv::Vec2f>(0, r*width + c) = cv::Vec2f((float)r, (float)c);
      }
  }

  cv::Mat k = cv::Mat::eye(3, 3, CV_32F);
  k.at<float>(0,0) = dev->getIrCameraParams().fx;
  k.at<float>(1,1) = dev->getIrCameraParams().fy;
  k.at<float>(0,2) = dev->getIrCameraParams().cx;
  k.at<float>(1,2) = dev->getIrCameraParams().cy;

  cv::Mat dist_coeffs = cv::Mat::zeros(1, 8, CV_32F);
  dist_coeffs.at<float>(0,0) = dev->getIrCameraParams().k1;
  dist_coeffs.at<float>(0,1) = dev->getIrCameraParams().k2;
  dist_coeffs.at<float>(0,2) = dev->getIrCameraParams().p1;
  dist_coeffs.at<float>(0,3) = dev->getIrCameraParams().p2;
  dist_coeffs.at<float>(0,4) = dev->getIrCameraParams().k3;

  cv::Mat new_camera_matrix = cv::getOptimalNewCameraMatrix(k, dist_coeffs, cv::Size2i(height,width), 0.0);

  cv::undistortPoints(cv_img_cords, cv_img_corrected_cords, k, dist_coeffs, cv::noArray(), new_camera_matrix);

  

  Mat xycords = cv_img_corrected_cords;

  float x = 0.0f, y = 0.0f;

  while(!protonect_shutdown)
  {
    listener.waitForNewFrame(frames);
    libfreenect2::Frame *rgb = frames[libfreenect2::Frame::Color];
    libfreenect2::Frame *ir = frames[libfreenect2::Frame::Ir];
    libfreenect2::Frame *depth = frames[libfreenect2::Frame::Depth];

    Mat depth_image = cv::Mat(depth->height, depth->width, CV_32FC1, depth->data) / 4500.0f;
    //cv::imshow("rgb", cv::Mat(rgb->height, rgb->width, CV_8UC4, rgb->data));
    //cv::imshow("ir", cv::Mat(ir->height, ir->width, CV_32FC1, ir->data) / 20000.0f);
    CvPoint3D64f *xyz;
    float* ptr = (float*) (depth_image.data);
    uint pixel_count = depth_image.rows * depth_image.cols;
    for (uint i = 0; i < pixel_count; ++i)
    {
        cv::Vec2f xy = xycords.at<cv::Vec2f>(0, i);
        x = xy[1]; y = xy[0];
        xyz[i].z = (static_cast<float>(*ptr)) / (1000.0f); // Convert from mm to meters
        xyz[i].x = (x - dev->getIrCameraParams().cx) * xyz[i].z / dev->getIrCameraParams().fx;
        xyz[i].y = (y - dev->getIrCameraParams().cy) * xyz[i].z / dev->getIrCameraParams().fy;
        //CvPoint3D64f point;
        //point.z = (static_cast<float>(*ptr)) / (1000.0f); // Convert from mm to meters
        //point.x = (x - dev->getIrCameraParams().cx) * point.z / dev->getIrCameraParams().fx;
        //point.y = (y - dev->getIrCameraParams().cy) * point.z / dev->getIrCameraParams().fy;
        //cout << "v " << point.x << " " << point.y << " " << point.z << endl;
        ++ptr;
    }

    static IplImage *p, *v, *m;
    static int width, height, cx, cy;
    double matrix[3][3], imatrix[3][3], background;

    background = 0.000199578;
    width = (int)(X_WIDTH*RESOLUTION);
    height = (int)(X_WIDTH*RESOLUTION);

    p = cvCreateImage(cvSize(width, height), IPL_DEPTH_64F, 1);
    m = cvCreateImage(cvSize(width, height), IPL_DEPTH_8U, 1);


    //computeRotationMatrix(matrix, imatrix, 0, -20*0.017453293, 0);
    //compute_projection(p, m, xyz, pixel_count, matrix, background);
    //Mat projecao= cv::cvarrToMat(p); 
    //cv::imshow("depth", projecao);

    //registration->apply(rgb,depth,&undistorted,&registered);

    //cv::imshow("undistorted", cv::Mat(undistorted.height, undistorted.width, CV_32FC1, undistorted.data) / 4500.0f);
    //cv::imshow("registered", cv::Mat(registered.height, registered.width, CV_8UC4, registered.data));

    int key = cv::waitKey(1);
    protonect_shutdown = protonect_shutdown || (key > 0 && ((key & 0xFF) == 27)); // shutdown on escape

    listener.release(frames);
    //libfreenect2::this_thread::sleep_for(libfreenect2::chrono::milliseconds(100));
  }

  // TODO: restarting ir stream doesn't work!
  // TODO: bad things will happen, if frame listeners are freed before dev->stop() :(
  dev->stop();
  dev->close();

  delete registration;

  return 0;
}
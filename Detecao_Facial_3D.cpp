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

const string PATH_CASCADE_FACE = "/home/matheusm/Cascades/ALL_Spring2003_3D.xml";
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
#define X_WIDTH 2800.0            // Orthogonal projection width - in mm
#define Y_WIDTH 2600.0            // Orthogonal projection height - in mm
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
    //cout << xyz[i].x << " " << xyz[i].y << " " << xyz[i].z << endl;
    j = cy-cvRound(xyz[i].x*matrix[1][0]+xyz[i].y*matrix[1][1]+xyz[i].z*matrix[1][2]);
    k = cx+cvRound(xyz[i].x*matrix[0][0]+xyz[i].y*matrix[0][1]+xyz[i].z*matrix[0][2]);
    d = xyz[i].x*matrix[2][0]+xyz[i].y*matrix[2][1]+xyz[i].z*matrix[2][2];
    //cout << j << " " << k << endl;
    
    //cout << d << endl;

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
// paraemtros da camera do Kinect
float fx;
float fy;
float cx;
float cy;
float k1;
float k2;
float p1;
float p2;
float k3;
//funcao que converte uma coordenada 3D em coordenada da imagem original
void xyz2depth(CvPoint3D64f *pt, double *i, double *j, double *s, Mat xycords) {
  double z, x, y;

  x = -(fx * pt->x)/pt->z + cx;
  y = (fy * pt->y)/pt->z - cy;
  *i = x;
  *j = y;
  *s = 20.0;
  //formula que converte um pixel da imagem original em coordenada 3D
  /*cv::Vec2f xy = xycords.at<cv::Vec2f>(0, i);
  x = xy[1]; y = xy[0];
  xyz[i].z = -(static_cast<float>(*ptr)) * (1000.0f); // Converte metros pra mm
  xyz[i].x = -(x - cx) * xyz[i].z / fx;
  xyz[i].y = (y - cy) * xyz[i].z / fy;*/
}

vector<Vec4d> face_detection(Mat &amDepth) {
  int liWidth = 512;
  int liHeight = 424;
  int liPwidth, liPheight;
  static CvPoint3D64f *xyz, *list, *clist;
  CvPoint3D64f avg;
  static CvHaarClassifierCascade *face_cascade;
  uint pixel_count = amDepth.rows * amDepth.cols;
  double menorX = 999999.0, menorY = 999999.0, menor = 999999.0;
  double maiorX = 0.0, maiorY = 0.0, maior = 0.0;
  static IplImage *p, *m, *sum, *sqsum, *tiltedsum, *msum, *sumint, *tiltedsumint;;
  static int width, height, cx, cy;
  double matrix[3][3], imatrix[3][3], background, X, Y, Z;
  float x = 0.0f, y = 0.0f;

  liPwidth = (int)(X_WIDTH*RESOLUTION);
  liPheight = (int)(X_WIDTH*RESOLUTION);

  p = cvCreateImage(cvSize(liPwidth, liPheight), IPL_DEPTH_64F, 1);
  m = cvCreateImage(cvSize(liPwidth, liPheight), IPL_DEPTH_8U, 1);

  //inicializacao do sistema de coordenadas
  cv::Mat cv_img_cords = cv::Mat(1, liWidth*liHeight, CV_32FC2);
  Mat cv_img_corrected_cords;
  for (int r = 0; r < liHeight; ++r) {
      for (int c = 0; c < liWidth; ++c) {
          cv_img_cords.at<cv::Vec2f>(0, r*liWidth + c) = cv::Vec2f((float)r, (float)c);
      }
  }

  cv::Mat lmK = cv::Mat::eye(3, 3, CV_32F);
  lmK.at<float>(0,0) = fx;
  lmK.at<float>(1,1) = fy;
  lmK.at<float>(0,2) = cx;
  lmK.at<float>(1,2) = cy;

  cv::Mat dist_coeffs = cv::Mat::zeros(1, 8, CV_32F);
  dist_coeffs.at<float>(0,0) = k1;
  dist_coeffs.at<float>(0,1) = k2;
  dist_coeffs.at<float>(0,2) = p1;
  dist_coeffs.at<float>(0,3) = p2;
  dist_coeffs.at<float>(0,4) = k3;

  cv::Mat new_camera_matrix = cv::getOptimalNewCameraMatrix(lmK, dist_coeffs, cv::Size2i(liHeight,liWidth), 0.0);
  cv::undistortPoints(cv_img_cords, cv_img_corrected_cords, lmK, dist_coeffs, cv::noArray(), new_camera_matrix);
  Mat xycords = cv_img_corrected_cords;
  //inicializacao das variaveis da detecao
  face_cascade = (CvHaarClassifierCascade *) cvLoad("/home/matheusm/Cascades/ALL_Spring2003_3D.xml", 0, 0, 0);
  sum = cvCreateImage(cvSize(width+1, height+1), IPL_DEPTH_64F, 1);
  sqsum = cvCreateImage(cvSize(width+1, height+1), IPL_DEPTH_64F, 1);
  tiltedsum = cvCreateImage(cvSize(width+1, height+1), IPL_DEPTH_64F, 1);
  sumint = cvCreateImage(cvSize(width+1, height+1), IPL_DEPTH_32S, 1);
  tiltedsumint = cvCreateImage(cvSize(width+1, height+1), IPL_DEPTH_32S, 1);
  msum = cvCreateImage(cvSize(width+1, height+1), IPL_DEPTH_32S, 1);

  list = (CvPoint3D64f *) malloc(2000*sizeof(CvPoint3D64f));
  clist = list+1000;
  //conversao de profundidade para coordenadas em 3D na imagem inteira
  xyz = (CvPoint3D64f *) malloc(SIZE*sizeof(CvPoint3D64f));
  float* ptr = (float*) (amDepth.data);
  for(uint i = 0; i < pixel_count; ++i)
  {
      cv::Vec2f xy = xycords.at<cv::Vec2f>(0, i);
      x = xy[1]; y = xy[0];
      xyz[i].z = -(static_cast<float>(*ptr)) * (1000.0f); // Converte metros pra mm
      xyz[i].x = -(x - cx) * xyz[i].z / fx;
      xyz[i].y = (y - cy) * xyz[i].z / fy;
      ++ptr;
      if(xyz[i].z < menor)
        menor = xyz[i].z;
  }
  background = menor;
  //calculo da imagem de projecao
  computeRotationMatrix(matrix, imatrix, 0, 0, 0);
  compute_projection(p, m, xyz, pixel_count, matrix, background);
  menor = 999999.0; 
  for(int i = 0; i < liPwidth; i++) {
    for(int j = 0; j < liHeight; j++) {
      double x = CV_IMAGE_ELEM(p, double, i, j);
      if(x > maior)
        maior = x;
      if(x < menor && x != 0)
        menor = x;
    }
  }
  //visualizacao da imagem de projecao
  #if 1
  double a, b;
  a = 255/(maior-menor);
  b = 1 - (menor * a);
  for(int i = 0; i < width; i++) {
    for(int j = 0; j < height; j++) {
      x = CV_IMAGE_ELEM(p, double, i, j);
      if(x != 0)
        CV_IMAGE_ELEM(p, double, i, j) = (x * a) + b;
    }
  }
  Mat projecao= cv::cvarrToMat(p); 
  
  Mat1b lmX(projecao.rows, projecao.cols);
  for(int i = 0; i < projecao.rows; i++)
    for(int j = 0; j < projecao.cols; j++) 
      lmX.at<uint8_t>(i, j) = projecao.at<double>(i, j);
  Mat colored;
  applyColorMap(lmX, colored, COLORMAP_JET);
  cv::imshow("input original2", colored);
  #endif
  //detecao da face
  cvIntegral(p, sum, sqsum, tiltedsum);
  cvIntegral(m, msum, NULL, NULL);
  int i, j, k = 0, l, n, aX, aY, aZ;
  for(i=0; i < height+1; i++)
    for(j=0; j < width+1; j++) {
      CV_IMAGE_ELEM(sumint, int, i, j) = CV_IMAGE_ELEM(sum, double, i, j);
      CV_IMAGE_ELEM(tiltedsumint, int, i, j) = CV_IMAGE_ELEM(tiltedsum, double, i, j);
    }

  cvSetImagesForHaarClassifierCascade(face_cascade, sumint, sqsum, tiltedsumint, 1.0);

  for(i=0; i < height-20; i++)
    for(j=0; j < width-20; j++)
      if(CV_IMAGE_ELEM(msum, int, i+FACE_SIZE, j+FACE_SIZE)-CV_IMAGE_ELEM(msum, int, i, j+FACE_SIZE)-CV_IMAGE_ELEM(msum, int, i+FACE_SIZE, j)+CV_IMAGE_ELEM(msum, int, i, j) == 441)
        if(cvRunHaarClassifierCascade(face_cascade, cvPoint(j,i), 0) > 0) {
          X = (j+FACE_HALF_SIZE-cx)/RESOLUTION;
          Y = (cy-i-FACE_HALF_SIZE)/RESOLUTION;
          Z = (CV_IMAGE_ELEM(sum, double, i+FACE_HALF_SIZE+6, j+FACE_HALF_SIZE+6)-CV_IMAGE_ELEM(sum, double, i+FACE_HALF_SIZE-5, j+FACE_HALF_SIZE+6)-CV_IMAGE_ELEM(sum, double, i+FACE_HALF_SIZE+6, j+FACE_HALF_SIZE-5)+CV_IMAGE_ELEM(sum, double, i+FACE_HALF_SIZE-5, j+FACE_HALF_SIZE-5))/121.0/RESOLUTION;

          list[k].x = X*imatrix[0][0]+Y*imatrix[0][1]+Z*imatrix[0][2];
          list[k].y = X*imatrix[1][0]+Y*imatrix[1][1]+Z*imatrix[1][2];
          list[k].z = X*imatrix[2][0]+Y*imatrix[2][1]+Z*imatrix[2][2];
          k++;
        }
  // Merge multiple detections
  vector<Vec4d> r;
  Vec4d tmp;
  while(k > 0) {
    avg.x = clist[0].x = list[0].x;
    avg.y = clist[0].y = list[0].y;
    avg.z = clist[0].z = list[0].z;
    xyz2depth(&avg, &tmp[1], &tmp[0], &tmp[2], xycords);
    tmp[3] = j;
    r.push_back(tmp);
    k--;
    avg.x = clist[0].x = list[0].x;
    avg.y = clist[0].y = list[0].y;
    avg.z = clist[0].z = list[0].z;
    list[0].x = DBL_MAX;

    j=1;
    for(l=0; l < j; l++)
      for(i=1; i < k; i++)
        if(list[i].x != DBL_MAX) {
          X = sqrt(pow(list[i].x-clist[l].x, 2.0)+pow(list[i].y-clist[l].y, 2.0)+pow(list[i].z-clist[l].z, 2.0));
          if(X < 50.0) {
            avg.x += clist[j].x = list[i].x;
            avg.y += clist[j].y = list[i].y;
            avg.z += clist[j].z = list[i].z;
            list[i].x = DBL_MAX;
            j++;
          }
        }

    avg.x /= j;
    avg.y /= j;
    avg.z /= j;

    xyz2depth(&avg, &tmp[1], &tmp[0], &tmp[2], xycords);
    tmp[3] = j;
    r.push_back(tmp);

    j=0;
    for(i=1; i < k; i++)
      if(list[i].x != DBL_MAX) {
        list[j].x = list[i].x;
        list[j].y = list[i].y;
        list[j].z = list[i].z;
        j++;
      }
    k=j;
  }
  return r;
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

  dev->setColorFrameListener(&listener);
  dev->setIrAndDepthFrameListener(&listener);
  dev->start();

  std::cout << "device serial: " << dev->getSerialNumber() << std::endl;
  std::cout << "device firmware: " << dev->getFirmwareVersion() << std::endl;

  bool shutdown = true;
  //parametros da camera
   fx = dev->getIrCameraParams().fx;
   fy = dev->getIrCameraParams().fy;
   cx = dev->getIrCameraParams().cx;
   cy = dev->getIrCameraParams().cy;
   k1 = dev->getIrCameraParams().k1;
   k2 = dev->getIrCameraParams().k2;
   p1 = dev->getIrCameraParams().p1;
   p2 = dev->getIrCameraParams().p2;
   k3 = dev->getIrCameraParams().k3;

  while(!protonect_shutdown)
  {
    listener.waitForNewFrame(frames);
    libfreenect2::Frame *depth = frames[libfreenect2::Frame::Depth];

    Mat lmDepth = cv::Mat(depth->height, depth->width, CV_32FC1, depth->data) / 4500.0f;
    vector<Vec4d> faces;

    faces = face_detection(lmDepth);
    // BEGIN: Visualization
    for(int i=0; i < faces.size(); i++)
      rectangle(lmDepth, Point(faces[i][0]-faces[i][2],faces[i][1]-faces[i][2]), Point(faces[i][0]+faces[i][2],faces[i][1]+faces[i][2]), CV_RGB(0,255,0), 2, 8, 0);
    // END: Visualization
    cv::imshow("Input Original", lmDepth);
    int key = cv::waitKey(1);
    protonect_shutdown = protonect_shutdown || (key > 0 && ((key & 0xFF) == 27)); // shutdown on escape

    listener.release(frames);
  }
  dev->stop();
  dev->close();

  return 0;
}

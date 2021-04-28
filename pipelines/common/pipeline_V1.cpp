
#include "Halide.h"
#include "halide_image_io.h"
#include <opencv2/opencv.hpp>
#include <stdio.h>
#include <math.h>
#include "../common/pipe_stages.h"
#include "../common/ImgPipeConfig.h"
#include "../common/LoadCamModel.h"
#include "../common/MatrixOps.h"
#include <ctime>
#include <omp.h>
// Pipeline V1 
// 
// Test type: 
// Full Reverse
// 
// Stages:
// Rto, Rg, Rtr, Renos, Remos

int main(int argc, char **argv) {

  using namespace std;

  clock_t startTime,endTime;
  startTime = clock();
  // Inform user of usage method
  if ( argc != 3 )
  {
      printf("usage: \n./convert path/to/in/image out/image/dir\n");
      return -1;
  }

  // Input image (path and name)
  const char * in_img_path = argv[1];

  // Output image (just path)
  const char * out_path    = argv[2];

  ///////////////////////////////////////////////////////////////////////////////////////
  // Import and format model data

  // Declare model parameters
  vector<vector<float>> Ts, Tw, TsTw;
  vector<vector<float>> rev_ctrl_pts, rev_weights, rev_coefs;
  vector<vector<float>> ctrl_pts, weights, coefs;
  vector<vector<float>> rev_tone;

  // Load model parameters from file
  // NOTE: Ts, Tw, and TsTw read only forward data
  // ctrl_pts, weights, and coefs are either forward or backward
  // tone mapping is always backward
  // This is due to the the camera model format
  Ts            = get_Ts       (cam_model_path);
  Tw            = get_Tw       (cam_model_path, wb_index);
  TsTw          = get_TsTw     (cam_model_path, wb_index);
  rev_ctrl_pts  = get_ctrl_pts (cam_model_path, num_ctrl_pts, 0);
  rev_weights   = get_weights  (cam_model_path, num_ctrl_pts, 0);
  rev_coefs     = get_coefs    (cam_model_path, num_ctrl_pts, 0);
  ctrl_pts      = get_ctrl_pts (cam_model_path, num_ctrl_pts, 1);
  weights       = get_weights  (cam_model_path, num_ctrl_pts, 1);
  coefs         = get_coefs    (cam_model_path, num_ctrl_pts, 1);
  rev_tone      = get_rev_tone (cam_model_path);

  // Take the transpose of the color map and white balance transform for later use
  vector<vector<float>> TsTw_tran     = transpose_mat(TsTw);

  // Create an inverse of TsTw_tran
  vector<vector<float>> TsTw_tran_inv = inv_3x3mat(TsTw_tran);

  using namespace Halide;
  using namespace Halide::Tools;
  using namespace cv;

  // Convert backward control points to a Halide image
  int width  = rev_ctrl_pts[0].size();
  int length = rev_ctrl_pts.size();
  Image<float> rev_ctrl_pts_h(width,length);
  #pragma omp parallel for 
  for (int y=0; y<length; y++) {
  #pragma omp parallel for
    for (int x=0; x<width; x++) {
      rev_ctrl_pts_h(x,y) = rev_ctrl_pts[y][x];
    }
  }
  // Convert backward weights to a Halide image
  width  = rev_weights[0].size();
  length = rev_weights.size();
  Image<float> rev_weights_h(width,length);
  #pragma omp parallel for
  for (int y=0; y<length; y++) {
  #pragma omp parallel for
    for (int x=0; x<width; x++) {
      rev_weights_h(x,y) = rev_weights[y][x];
    }
  }

  // Convert control points to a Halide image
  width  = ctrl_pts[0].size();
  length = ctrl_pts.size();
  Image<float> ctrl_pts_h(width,length);
  #pragma omp parallel for
  for (int y=0; y<length; y++) {
  #pragma omp parallel for
    for (int x=0; x<width; x++) {
      ctrl_pts_h(x,y) = ctrl_pts[y][x];
    }
  }
  // Convert weights to a Halide image
  width  = weights[0].size();
  length = weights.size();
  Image<float> weights_h(width,length);
#pragma omp parallel for
  for (int y=0; y<length; y++) {
#pragma omp parallel for
    for (int x=0; x<width; x++) {
      weights_h(x,y) = weights[y][x];
    }
  }
  // Convert the reverse tone mapping function to a Halide image
  width  = 3;
  length = 256;
  Image<float> rev_tone_h(width,length);
#pragma omp parallel for
  for (int y=0; y<length; y++) {
#pragma omp parallel for
    for (int x=0; x<width; x++) {
      rev_tone_h(x,y) = rev_tone[y][x];
    }
  }
 
  ///////////////////////////////////////////////////////////////////////////////////////
  // Import and format input image

  // Declare image handle variables
  Var x, y, c;

  // Load input image 
  Image<uint8_t> input = load_image(in_img_path);

  ///////////////////////////////////////////////////////////////////////////////////////
  // Camera pipeline

  width  = input.width();
  int height = input.height();

  // Scale to 0-1 range and represent in floating point
  Func scale              = make_scale        ( &input);

  // Backward pipeline
  Func rev_tone_map       = make_rev_tone_map ( &scale,
                                                  &rev_tone_h );
  Func rev_gamut_map_ctrl = make_rbf_ctrl_pts ( &rev_tone_map,
                                                num_ctrl_pts,
                                                &rev_ctrl_pts_h,
                                                &rev_weights_h );
  Func rev_gamut_map_bias = make_rbf_biases   ( &rev_tone_map,
                                                &rev_gamut_map_ctrl,
                                                &rev_coefs );
  /*Func rev_transform      = make_transform    ( &rev_gamut_map_bias,
                                                &TsTw_tran_inv );*/

  //rev_tone_map.compute_root();
  //rev_gamut_map_ctrl.compute_root();
  rev_tone_map.compute_root();
  rev_gamut_map_ctrl.compute_root();

  //Image<float> opencv_in_image = rev_transform.realize(width, height, 3);
  Image<float> opencv_in_image = rev_gamut_map_bias.realize(width, height, 3);

  Mat opencv_in_mat = Image2Mat(&opencv_in_image);

 // OpenCV_renoise(&opencv_in_mat);

  OpenCV_remosaic(&opencv_in_mat);

  Image<float> opencv_out = Mat2Image(&opencv_in_mat);

  Func Image2Func         = make_Image2Func   ( &opencv_out );

  // Scale back to 0-255 and represent in 8 bit fixed point
  Func descale            = make_descale      ( &Image2Func );


  ///////////////////////////////////////////////////////////////////////////////////////
  // Scheduling

  // Use JIT compiler
  descale.compile_jit();
  Image<uint8_t> output = descale.realize(width,height,3);

  ////////////////////////////////////////////////////////////////////////
  // Save the output
  endTime = clock();
  //get the num of core
  int coreNum = omp_get_num_procs();
  cout<<"the num of core is:"<<coreNum<<endl;
  save_image(output, (std::string(out_path)+"output.png").c_str());
  endTime = clock();
  cout<<"The run time is:"<<(double)(endTime-startTime)/CLOCKS_PER_SEC << "s" <<endl;
  return 0;
}


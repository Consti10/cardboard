/*
 * Copyright 2019 Google Inc. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "hello_cardboard_app.h"
#include "../../../../sdk/util/matrix_4x4.h"

#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>

#include <array>
#include <cmath>
#include <fstream>
#include <GeometryBuilder/TexturedGeometry.hpp>
#include <Helper/GLBufferHelper.hpp>
#include <GeometryBuilder/ColoredGeometry.hpp>

namespace ndk_hello_cardboard {

namespace {

// The objects are about 1 meter in radius, so the min/max target distance are
// set so that the objects are always within the room (which is about 5 meters
// across) and the reticle is always closer than any objects.
constexpr float kMinTargetDistance = 2.5f;
constexpr float kMaxTargetDistance = 3.5f;
constexpr float kMinTargetHeight = 0.5f;
constexpr float kMaxTargetHeight = kMinTargetHeight + 3.0f;

constexpr float kDefaultFloorHeight = -1.7f;//-1.7f;

constexpr uint64_t kPredictionTimeWithoutVsyncNanos = 50000000;

// Angle threshold for determining whether the controller is pointing at the
// object.
constexpr float kAngleLimit = 0.2f;

// Number of different possible targets
constexpr int kTargetMeshCount = 3;

// Simple shaders to render .obj files without any lighting.
constexpr const char* kObjVertexShaders =
    R"glsl(
    uniform mat4 u_MVP;
    attribute vec4 a_Position;
    attribute vec2 a_UV;
    varying vec2 v_UV;

    void main() {
      v_UV = a_UV;
      gl_Position = u_MVP * a_Position;
    })glsl";

constexpr const char* kObjFragmentShaders =
    R"glsl(
    precision mediump float;
    varying vec2 v_UV;
    uniform sampler2D u_Texture;

    void main() {
      // The y coordinate of this sample's textures is reversed compared to
      // what OpenGL expects, so we invert the y coordinate.
      gl_FragColor = texture2D(u_Texture, vec2(v_UV.x, 1.0 - v_UV.y));
    })glsl";

}  // anonymous namespace


HelloCardboardApp::HelloCardboardApp(JavaVM* vm, jobject obj, jobject asset_mgr_obj)
    : distortionManager(DistortionManager::DISTORTION_MODE::RADIAL_2),
    head_tracker_(nullptr),
      lens_distortion_(nullptr),
      distortion_renderer_(nullptr),
      screen_params_changed_(false),
      device_params_changed_(false),
      screen_width_(0),
      screen_height_(0),
      depthRenderBuffer_(0),
      framebuffer_(0),
      texture_(0),
      obj_program_(0),
      obj_position_param_(0),
      obj_uv_param_(0),
      obj_modelview_projection_param_(0),
      target_object_meshes_(kTargetMeshCount),
      target_object_not_selected_textures_(kTargetMeshCount),
      target_object_selected_textures_(kTargetMeshCount),
      cur_target_object_(RandomUniformInt(kTargetMeshCount)) {
  JNIEnv* env;
  vm->GetEnv((void**)&env, JNI_VERSION_1_6);
  java_asset_mgr_ = env->NewGlobalRef(asset_mgr_obj);
  asset_mgr_ = AAssetManager_fromJava(env, asset_mgr_obj);
  Cardboard_initializeAndroid(vm, obj);
  head_tracker_ = CardboardHeadTracker_create();
}

HelloCardboardApp::~HelloCardboardApp() {
  CardboardHeadTracker_destroy(head_tracker_);
  CardboardLensDistortion_destroy(lens_distortion_);
  CardboardDistortionRenderer_destroy(distortion_renderer_);
}

void HelloCardboardApp::OnSurfaceCreated(JNIEnv* env) {
  const int obj_vertex_shader =
      LoadGLShader(GL_VERTEX_SHADER, kObjVertexShaders);
  const int obj_fragment_shader =
      LoadGLShader(GL_FRAGMENT_SHADER, kObjFragmentShaders);

  obj_program_ = glCreateProgram();
  glAttachShader(obj_program_, obj_vertex_shader);
  glAttachShader(obj_program_, obj_fragment_shader);
  glLinkProgram(obj_program_);
  glUseProgram(obj_program_);

  CHECKGLERROR("Obj program");

  obj_position_param_ = glGetAttribLocation(obj_program_, "a_Position");
  obj_uv_param_ = glGetAttribLocation(obj_program_, "a_UV");
  obj_modelview_projection_param_ = glGetUniformLocation(obj_program_, "u_MVP");

  CHECKGLERROR("Obj program params");

  HELLOCARDBOARD_CHECK(room_.Initialize(env, asset_mgr_, "CubeRoom.obj",
                                 obj_position_param_, obj_uv_param_));
  HELLOCARDBOARD_CHECK(
      room_tex_.Initialize(env, java_asset_mgr_, "CubeRoom_BakedDiffuse.png"));
  HELLOCARDBOARD_CHECK(target_object_meshes_[0].Initialize(
      env, asset_mgr_, "Icosahedron.obj", obj_position_param_, obj_uv_param_));
  HELLOCARDBOARD_CHECK(target_object_not_selected_textures_[0].Initialize(
      env, java_asset_mgr_, "Icosahedron_Blue_BakedDiffuse.png"));
  HELLOCARDBOARD_CHECK(target_object_selected_textures_[0].Initialize(
      env, java_asset_mgr_, "Icosahedron_Pink_BakedDiffuse.png"));
  HELLOCARDBOARD_CHECK(target_object_meshes_[1].Initialize(
      env, asset_mgr_, "QuadSphere.obj", obj_position_param_, obj_uv_param_));
  HELLOCARDBOARD_CHECK(target_object_not_selected_textures_[1].Initialize(
      env, java_asset_mgr_, "QuadSphere_Blue_BakedDiffuse.png"));
  HELLOCARDBOARD_CHECK(target_object_selected_textures_[1].Initialize(
      env, java_asset_mgr_, "QuadSphere_Pink_BakedDiffuse.png"));
  HELLOCARDBOARD_CHECK(target_object_meshes_[2].Initialize(
      env, asset_mgr_, "TriSphere.obj", obj_position_param_, obj_uv_param_));
  HELLOCARDBOARD_CHECK(target_object_not_selected_textures_[2].Initialize(
      env, java_asset_mgr_, "TriSphere_Blue_BakedDiffuse.png"));
  HELLOCARDBOARD_CHECK(target_object_selected_textures_[2].Initialize(
      env, java_asset_mgr_, "TriSphere_Pink_BakedDiffuse.png"));

  // Target object first appears directly in front of user.
  model_target_ = GetTranslationMatrix({0.0f, 1.5f, kMinTargetDistance});
  //
  //glGenTextures(1,&testTexture);
  testTexture.Initialize(env,java_asset_mgr_,"basicgrid_middlecross.png");

  glProgramVC=new GLProgramVC(nullptr);
  glProgramVCDistortion=new GLProgramVC(&distortionManager);
  glProgramVC2D=new GLProgramVC(nullptr,true);
  float tesselatedRectSize=2.0; //6.2f
  const float offsetY=0.0f;
  auto tmp=ColoredGeometry::makeTesselatedColoredRectLines(LINE_MESH_TESSELATION_FACTOR,{-tesselatedRectSize/2.0f,-tesselatedRectSize/2.0f+offsetY,-2},tesselatedRectSize,tesselatedRectSize,Color::BLUE);
  glGenBuffers(1,&glBufferVC);
  nColoredVertices=GLBufferHelper::allocateGLBufferStatic(glBufferVC,tmp);
  tmp=ColoredGeometry::makeTesselatedColoredRectLines(LINE_MESH_TESSELATION_FACTOR,{-tesselatedRectSize/2.0f,-tesselatedRectSize/2.0f+offsetY,-2},tesselatedRectSize,tesselatedRectSize,Color::GREEN);
    glGenBuffers(1,&glBufferVCX);
    GLBufferHelper::allocateGLBufferStatic(glBufferVCX,tmp);
  coordinateSystemLines.initializeGL();
  /*glProgramTexture=new GLProgramTexture(false, nullptr,false);
  glGenBuffers(1,&glBufferTextured);
  const float sizeX=1.0f;
  const float sizeY=1.0f;
  const auto tesselatedVideoCanvas=TexturedGeometry::makeTesselatedVideoCanvas2(glm::vec3(-sizeX/2.0f,-sizeY/2.0f,0),
                                                                                sizeX,sizeY, TEXTURE_TESSELATION_FACTOR, 0.0f,1.0f);
  GLBufferHelper::allocateGLBufferStatic(glBufferTextured,tesselatedVideoCanvas);
  nTexturedVertices=tesselatedVideoCanvas.size();*/
  CHECKGLERROR("OnSurfaceCreated");
}

void HelloCardboardApp::SetScreenParams(int width, int height) {
  screen_width_ = width;
  screen_height_ = height;
  screen_params_changed_ = true;
}

void HelloCardboardApp::OnDrawFrame() {
  if (!UpdateDeviceParams()) {
    return;
  }

  // Update Head Pose.
  head_view_ = GetPose();

  // Incorporate the floor height into the head_view
  //head_view_ =
  //    head_view_ * GetTranslationMatrix({0.0f, kDefaultFloorHeight, 0.0f});

  // Bind buffer
  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);

  glEnable(GL_DEPTH_TEST);
  glEnable(GL_CULL_FACE);
  glDisable(GL_SCISSOR_TEST);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  // Draw eyes views
  for (int eye = 0; eye < 2; ++eye) {
      glViewport(eye == kLeft ? 0 : screen_width_ / 2, 0, screen_width_ / 2,
                 screen_height_);
    //glViewport(0 , 0, screen_width_ ,screen_height_);

    Matrix4x4 eye_matrix = GetMatrixFromGlArray(eye_matrices_[eye]);
    cEyeViews[eye] = eye_matrix * head_view_;

    Matrix4x4 projection_matrix =
        GetMatrixFromGlArray(projection_matrices_[eye]);
    Matrix4x4 modelview_target = cEyeViews[eye] * model_target_;
    modelview_projection_target_ = projection_matrix * modelview_target;
    modelview_projection_room_ = projection_matrix * cEyeViews[eye];

    // Draw room and target
    DrawWorld(eye);
  }

  // Render
  CardboardDestortionRenderer_renderEyeToDisplay(
      distortion_renderer_, 0, screen_width_, screen_height_,
      &left_eye_texture_description_, &right_eye_texture_description_);

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  //render same mesh with disortion correction
    for (int eye = 0; eye < 2; ++eye) {
        glViewport(eye == kLeft ? 0 : screen_width_ / 2, 0, screen_width_ / 2,
                   screen_height_);
        //glViewport(0,0,screen_width_,screen_height_);
        /*glProgramVC2D->beforeDraw(glBufferVC2[eye]);
        std::array<float, 16> cEyeView1=cEyeViews[0].ToGlArray();
        std::array<float, 16> cEyeView2=cEyeViews[1].ToGlArray();
        //glm::value_ptr(glm::mat4(1.0)),glm::value_ptr(glm::mat4(1.0))
        glProgramVC2D->draw(eye==0 ? cEyeView1.data() : cEyeView2.data(),glm::value_ptr(mProjectionM),0,nColoredVertices2,GL_LINES);
        glProgramVC2D->afterDraw();*/
        //coordinateSystemLines.drawGL(glProgramVC,0,0,0,0);

        distortionManager.leftEye=eye==kLeft;

      glProgramVCDistortion->beforeDraw(glBufferVCX);
      glLineWidth(2.0f);
      auto* viewM=glm::value_ptr(mViewM[eye]);
      //auto* viewM=cEyeViews[eye].ToGlArray().data();
      auto* projM=glm::value_ptr(mProjectionM[eye]);
      //auto* projM=projection_matrices_[eye];
      glProgramVCDistortion->draw(viewM,projM,0,nColoredVertices,GL_LINES);
      glProgramVCDistortion->afterDraw();

        /*glProgramVCDistortion->beforeDraw(glBufferVC);
        glProgramVCDistortion->draw(cEyeView1.data(),projection_matrices_[0],0,nColoredVertices,GL_LINES);
        glProgramVCDistortion->afterDraw();*/
    }

  CHECKGLERROR("onDrawFrame");
}

void HelloCardboardApp::OnTriggerEvent() {
  if (IsPointingAtTarget()) {
    HideTarget();
  }
}

void HelloCardboardApp::OnPause() { CardboardHeadTracker_pause(head_tracker_); }

void HelloCardboardApp::OnResume() {
  CardboardHeadTracker_resume(head_tracker_);

  // Parameters may have changed.
  device_params_changed_ = true;

  // Check for device parameters existence in external storage. If they're
  // missing, we must scan a Cardboard QR code and save the obtained parameters.
  uint8_t* buffer;
  int size;
  CardboardQrCode_getSavedDeviceParams(&buffer, &size);
  if (size == 0) {
    SwitchViewer();
  }
  CardboardQrCode_destroy(buffer);
}

void HelloCardboardApp::SwitchViewer() {
  CardboardQrCode_scanQrCodeAndSaveDeviceParams();
}

static glm::mat4 perspective(std::array<float,4> fov,float zNear, float zFar){
  const float xLeft = -std::tan(fov[0] * M_PI / 180.0f) * zNear;
  const float xRight = std::tan(fov[1] * M_PI / 180.0f) * zNear;
  const float yBottom = -std::tan(fov[2] * M_PI / 180.0f) * zNear;
  const float yTop = std::tan(fov[3] * M_PI / 180.0f) * zNear;
  return glm::frustum(xLeft,xRight,yBottom,yTop,zNear,zFar);
}

bool HelloCardboardApp::UpdateDeviceParams() {
  // Checks if screen or device parameters changed
  if (!screen_params_changed_ && !device_params_changed_) {
    return true;
  }

  // Get saved device parameters
  uint8_t* buffer;
  int size;
  CardboardQrCode_getSavedDeviceParams(&buffer, &size);

  // If there are no parameters saved yet, returns false.
  if (size == 0) {
    return false;
  }

  CardboardLensDistortion_destroy(lens_distortion_);
  lens_distortion_ = CardboardLensDistortion_create(
      buffer, size, screen_width_, screen_height_);


    //const auto polynomialRadialDistortion=static_cast<cardboard::PolynomialRadialDistortion*>(CardboardLensDistortion_getPolynomialRadialDistortion(lens_distortion_));
    //const auto mInverse=polynomialRadialDistortion->getApproximateInverseDistortion(MAX_RAD_SQ,6);

//cLensDistortion= static_cast<cardboard::LensDistortion*>(lens_distortion_);
  //cardboard::DeviceParams deviceParams;
  MDeviceParams mDP=CreateMLensDistortion(buffer,size,screen_width_,screen_height_);
  auto polynomialRadialDistortion=MPolynomialRadialDistortion(mDP.radial_distortion_params);

  const auto GetYEyeOffsetMeters= MLensDistortion::GetYEyeOffsetMeters(mDP.vertical_alignment,
                                                                       mDP.tray_to_lens_distance,
                                                                       mDP.screen_height_meters);
  const auto fovLeft= MLensDistortion::CalculateFov(mDP.device_fov_left, GetYEyeOffsetMeters,
                                                mDP.screen_to_lens_distance,
                                                mDP.inter_lens_distance,
                                                polynomialRadialDistortion,
                                                mDP.screen_width_meters, mDP.screen_height_meters);
  const auto fovRight=MLensDistortion::reverseFOV(fovLeft);

  std::array<MLensDistortion::ViewportParams,2> screen_params;
  std::array<MLensDistortion::ViewportParams,2> texture_params;

  MLensDistortion::CalculateViewportParameters_NDC(kLeft, GetYEyeOffsetMeters,
                                                 mDP.screen_to_lens_distance,
                                                 mDP.inter_lens_distance, fovLeft,
                                                 mDP.screen_width_meters, mDP.screen_height_meters,
                                                 &screen_params[0], &texture_params[0]);
  MLensDistortion::CalculateViewportParameters_NDC(kRight, GetYEyeOffsetMeters,
                                                 mDP.screen_to_lens_distance,
                                                 mDP.inter_lens_distance, fovRight,
                                                 mDP.screen_width_meters, mDP.screen_height_meters,
                                                 &screen_params[1], &texture_params[1]);

  const float MAX_X_VALUE1=1*texture_params[0].width - texture_params[0].x_eye_offset;
  const float MAX_X_VALUE2=1*texture_params[1].width - texture_params[1].x_eye_offset;
  const float MAX_Y_VALUE1=1*texture_params[0].height - texture_params[0].y_eye_offset;
  const float MAX_Y_VALUE2=1*texture_params[1].height- texture_params[1].y_eye_offset;
  const float MAX_RAD_SQ1=(float)(pow(MAX_X_VALUE1,2)+pow(MAX_Y_VALUE1,2));
  const float MAX_RAD_SQ2=(float)(pow(MAX_X_VALUE2,2)+pow(MAX_Y_VALUE2,2));
  float MAX_RAD_SQ=std::max(MAX_RAD_SQ1,MAX_RAD_SQ2);
  //const float MAX_RAD_SQ=1.0f;
  LOGD("Max Rad Sq%f",MAX_RAD_SQ);
  MAX_RAD_SQ=1.0f;

  const auto mInverse=polynomialRadialDistortion.getApproximateInverseDistortion(MAX_RAD_SQ,6);
  distortionManager.updateDistortion(mInverse,MAX_RAD_SQ,screen_params,texture_params);

  float tesselatedRectSize=1.0f;
  const auto tmp=ColoredGeometry::makeTesselatedColoredRectLines(LINE_MESH_TESSELATION_FACTOR,{0,0,0},1.0f,1.0f,Color::GREEN);

  std::vector<GLProgramVC::Vertex> tmp2;
  std::vector<GLProgramVC::Vertex> tmp3;
  for(const GLProgramVC::Vertex v : tmp){
    CardboardUv input={v.x,v.y};
    //const auto distortedP=CardboardLensDistortion_undistortedUvForDistortedUv(lens_distortion_,&input,kLeft);
    const auto distortedP=MLensDistortion::UndistortedUvForDistortedUv(polynomialRadialDistortion,screen_params[0],texture_params[0],{v.x,v.y});
    /*const auto distortedP= MLensDistortion::UndistortedNDCForDistortedNDC(mInverse,
                                                                          screen_params[0],
                                                                          texture_params[0],
                                                                          {v.x, v.y});*/
    GLProgramVC::Vertex newVertex;
    //newVertex.x=v.x;
    //newVertex.y=v.y;
    newVertex.x=distortedP[0]*4-1;
    newVertex.y=distortedP[1]*2-1;
    //newVertex.x=distortedP[0];
    //newVertex.y=distortedP[1];
    newVertex.z=v.z;
    newVertex.colorRGBA=v.colorRGBA;
    tmp2.push_back(newVertex);
    //const auto distortedP2=CardboardLensDistortion_undistortedUvForDistortedUv(lens_distortion_,&input,kRight);
    const auto distortedP2=MLensDistortion::UndistortedUvForDistortedUv(polynomialRadialDistortion,screen_params[1],texture_params[1],{v.x,v.y});
    /*const auto distortedP2= MLensDistortion::UndistortedNDCForDistortedNDC(
            mInverse, screen_params[1], texture_params[1], {v.x, v.y});*/
    newVertex.x=distortedP2[0]*4-3;
    newVertex.y=distortedP2[1]*2-1;
      //newVertex.x=distortedP2[0];
      //newVertex.y=distortedP2[1];
    tmp3.push_back(newVertex);
  }
  glGenBuffers(2,glBufferVC2);
  nColoredVertices2=GLBufferHelper::allocateGLBufferStatic(glBufferVC2[0],tmp2);
  nColoredVertices2=GLBufferHelper::allocateGLBufferStatic(glBufferVC2[1],tmp3);

  //LOGD("XXX%s",mInverse.toString().c_str());
  CardboardQrCode_destroy(buffer);

  //update the vddc matrices
  const float mViewPortW=screen_width_/2.0f;
  const float mViewPortH=screen_height_;
  //mProjectionM[0]=glm::perspective(glm::radians(80.0f),((float) mViewPortW)/((float)mViewPortH), MIN_Z_DISTANCE, MAX_Z_DISTANCE);
  mProjectionM[0]=perspective(fovLeft,0.1f,100.0f);
  mProjectionM[1]=perspective(fovRight,0.1f,100.0f);


  const float inter_lens_distance=mDP.inter_lens_distance;
  glm::vec3 cameraPos   = glm::vec3(0,0,CAMERA_POSITION);
  glm::vec3 cameraFront = glm::vec3(0.0F,0.0F,-1.0F);
  glm::mat4 eyeView=glm::mat4(1.0f);//glm::lookAt(cameraPos,cameraPos+cameraFront,glm::vec3(0,1,0));
  //mViewM[0]=glm::translate(eyeView,glm::vec3(inter_lens_distance/2.0f,0,0));
  //mViewM[1]=glm::translate(eyeView,glm::vec3(-inter_lens_distance/2.0f,0,0));
  mViewM[0]=glm::translate(eyeView,glm::vec3(inter_lens_distance*0.5f,0,0));
  mViewM[1]=glm::translate(eyeView,glm::vec3(-inter_lens_distance*0.5f,0,0));

  GlSetup();

  CardboardDistortionRenderer_destroy(distortion_renderer_);
  distortion_renderer_ = CardboardDistortionRenderer_create();

  CardboardMesh left_mesh;
  CardboardMesh right_mesh;
  CardboardLensDistortion_getDistortionMesh(lens_distortion_, kLeft,
                                            &left_mesh);
  CardboardLensDistortion_getDistortionMesh(lens_distortion_, kRight,
                                            &right_mesh);

  CardboardDistortionRenderer_setMesh(distortion_renderer_, &left_mesh, kLeft);
  CardboardDistortionRenderer_setMesh(distortion_renderer_, &right_mesh,
                                      kRight);

  // Get eye matrices
  CardboardLensDistortion_getEyeMatrices(
      lens_distortion_, projection_matrices_[0], eye_matrices_[0], kLeft);
  CardboardLensDistortion_getEyeMatrices(
      lens_distortion_, projection_matrices_[1], eye_matrices_[1], kRight);

  screen_params_changed_ = false;
  device_params_changed_ = false;

  CHECKGLERROR("UpdateDeviceParams");

  return true;
}

void HelloCardboardApp::GlSetup() {
  LOGD("GL SETUP");

  if (framebuffer_ != 0) {
    GlTeardown();
  }

  // Create render texture.
  glGenTextures(1, &texture_);
  glBindTexture(GL_TEXTURE_2D, texture_);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, screen_width_, screen_height_, 0,
               GL_RGB, GL_UNSIGNED_BYTE, 0);


  left_eye_texture_description_.texture = texture_;
  //left_eye_texture_description_.texture=testTexture.texture_id_;
  left_eye_texture_description_.layer = 0;
  left_eye_texture_description_.left_u = 0;
  left_eye_texture_description_.right_u = 0.5f;
  //left_eye_texture_description_.right_u = 1.0f;
  left_eye_texture_description_.top_v = 1;
  left_eye_texture_description_.bottom_v = 0;

  right_eye_texture_description_.texture = texture_;
  //right_eye_texture_description_.texture = testTexture.texture_id_;
  right_eye_texture_description_.layer = 0;
  right_eye_texture_description_.left_u = 0.5f;
  right_eye_texture_description_.right_u = 1;
  right_eye_texture_description_.top_v = 1;
  right_eye_texture_description_.bottom_v = 0;

  // Generate depth buffer to perform depth test.
  glGenRenderbuffers(1, &depthRenderBuffer_);
  glBindRenderbuffer(GL_RENDERBUFFER, depthRenderBuffer_);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, screen_width_,
                        screen_height_);
  CHECKGLERROR("Create Render buffer");

  // Create render target.
  glGenFramebuffers(1, &framebuffer_);
  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         texture_, 0);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                            GL_RENDERBUFFER, depthRenderBuffer_);

  CHECKGLERROR("GlSetup");
}

void HelloCardboardApp::GlTeardown() {
  if (framebuffer_ == 0) {
    return;
  }
  glDeleteRenderbuffers(1, &depthRenderBuffer_);
  depthRenderBuffer_ = 0;
  glDeleteFramebuffers(1, &framebuffer_);
  framebuffer_ = 0;
  glDeleteTextures(1, &texture_);
  texture_ = 0;

  CHECKGLERROR("GlTeardown");
}

Matrix4x4 HelloCardboardApp::GetPose() {
  std::array<float, 4> out_orientation;
  std::array<float, 3> out_position;
  long monotonic_time_nano = GetMonotonicTimeNano();
  monotonic_time_nano += kPredictionTimeWithoutVsyncNanos;
  CardboardHeadTracker_getPose(head_tracker_, monotonic_time_nano,
                               &out_position[0], &out_orientation[0]);
  //return GetTranslationMatrix(out_position) *
  //       Quatf::FromXYZW(&out_orientation[0]).ToMatrix();
         //use o default rotation of 0
  //return Quatf::FromXYZW(&out_orientation[0]).ToMatrix();
  return Quatf().ToMatrix();
}

void HelloCardboardApp::DrawWorld(const int eye) {
  DrawRoom();
  DrawTarget();
  glLineWidth(4.0f);
    glProgramVC->beforeDraw(glBufferVC);
    std::array<float, 16> cEyeView1=cEyeViews[eye].ToGlArray();
    glProgramVC->draw(cEyeView1.data(),projection_matrices_[eye],0,nColoredVertices,GL_LINES);
    glProgramVC->afterDraw();
    CHECKGLERROR("DrawDebug");
}

void HelloCardboardApp::DrawTarget() {
  glUseProgram(obj_program_);

  std::array<float, 16> target_array = modelview_projection_target_.ToGlArray();
  glUniformMatrix4fv(obj_modelview_projection_param_, 1, GL_FALSE,
                     target_array.data());

  if (IsPointingAtTarget()) {
    target_object_selected_textures_[cur_target_object_].Bind();
  } else {
    target_object_not_selected_textures_[cur_target_object_].Bind();
  }
  target_object_meshes_[cur_target_object_].Draw();

  CHECKGLERROR("DrawTarget");
}

void HelloCardboardApp::DrawRoom() {
  glUseProgram(obj_program_);

  std::array<float, 16> room_array = modelview_projection_room_.ToGlArray();
  glUniformMatrix4fv(obj_modelview_projection_param_, 1, GL_FALSE,
                     room_array.data());

  room_tex_.Bind();
  room_.Draw();
}

void HelloCardboardApp::HideTarget() {
  cur_target_object_ = RandomUniformInt(kTargetMeshCount);

  float angle = RandomUniformFloat(-M_PI, M_PI);
  float distance = RandomUniformFloat(kMinTargetDistance, kMaxTargetDistance);
  float height = RandomUniformFloat(kMinTargetHeight, kMaxTargetHeight);
  std::array<float, 3> target_position = {std::cos(angle) * distance, height,
                                          std::sin(angle) * distance};

  model_target_ = GetTranslationMatrix(target_position);
}

bool HelloCardboardApp::IsPointingAtTarget() {
  // Compute vectors pointing towards the reticle and towards the target object
  // in head space.
  Matrix4x4 head_from_target = head_view_ * model_target_;

  const std::array<float, 4> unit_quaternion = {0.f, 0.f, 0.f, 1.f};
  const std::array<float, 4> point_vector = {0.f, 0.f, -1.f, 0.f};
  const std::array<float, 4> target_vector = head_from_target * unit_quaternion;

  float angle = AngleBetweenVectors(point_vector, target_vector);
  return angle < kAngleLimit;
}

void HelloCardboardApp::DrawMeshVDDC() {

}

}  // namespace ndk_hello_cardboard

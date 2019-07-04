#include "online_calibration_alg.h"

void point2SphericalGrid(pcl::PointXYZI point, struct SensorConfiguration lidar_configuration, int& row, int& col);
void cartesian2SphericalInDegrees(float x, float y, float z, float& range, float& azimuth, float& elevation);
void fillGraps(cv::Mat& image);
float colorMap(float dist, float factor, int base);

OnlineCalibrationAlgorithm::OnlineCalibrationAlgorithm(void)
{
  pthread_mutex_init(&this->access_, NULL);
}

OnlineCalibrationAlgorithm::~OnlineCalibrationAlgorithm(void)
{
  pthread_mutex_destroy(&this->access_);
}

void OnlineCalibrationAlgorithm::config_update(Config& config, uint32_t level)
{
  this->lock();

  // save the current configuration
  this->config_ = config;

  this->unlock();
}

// OnlineCalibrationAlgorithm Public API
void OnlineCalibrationAlgorithm::sensorFusion(cv::Mat last_image, sensor_msgs::PointCloud2 msg,
                                              image_geometry::PinholeCameraModel cam_model, std::string frame_id,
                                              ros::Time acquisition_time, tf::TransformListener& tf_listener,
                                              cv::Mat& depth_map, cv::Mat& color_map, cv::Mat& plot_image)
{

  /****** variable declarations ******/
  int i, j, k;
  float factor_color = 60.0; //TODO: get from param
  int rows = last_image.rows;
  int cols = last_image.cols;
  int index[rows][cols];
  for (i = 0; i < cols; i++)
    for (j = 0; j < rows; j++)
      index[j][i] = NO_INDEX;

  /****** get transformation, and transform point cloud ******/
  sensor_msgs::PointCloud2 transform_pc;
  try
  {
    ros::Duration timeout(1.0 / 30);
    tf_listener.waitForTransform(cam_model.tfFrame(), frame_id, acquisition_time, timeout);
    pcl_ros::transformPointCloud(cam_model.tfFrame(), msg, transform_pc, tf_listener);
  }
  catch (tf::TransformException& ex)
  {
    ROS_WARN("[draw_frames] TF exception:\n%s", ex.what());
    return;
  }

  /****** generate new point clouds with PCL format ******/
  pcl::PointCloud<pcl::PointXYZI>::Ptr cloud_pcl(new pcl::PointCloud<pcl::PointXYZI>);
  pcl::PointCloud<pcl::PointXYZI>::Ptr cloud_pcl_orig(new pcl::PointCloud<pcl::PointXYZI>);
  pcl::PCLPointCloud2 aux_input;
  pcl_conversions::toPCL(transform_pc, aux_input);
  pcl::fromPCLPointCloud2(aux_input, *cloud_pcl);
  pcl_conversions::toPCL(msg, aux_input);
  pcl::fromPCLPointCloud2(aux_input, *cloud_pcl_orig);

  /****** get field of view and save index correspondence between cloud and image ******/
  float max_elevation = -10000.0;
  float min_elevation = 10000.0;
  float max_azimut = -10000.0;
  float min_azimut = 10000.0;
  for (size_t i = 0; i < cloud_pcl->points.size(); ++i)
  {
    if (cloud_pcl->points[i].z > 0.0)
    {
      // project into image plane
      cv::Point3d pt_cv(cloud_pcl->points[i].x, cloud_pcl->points[i].y, cloud_pcl->points[i].z);
      cv::Point2d uv;
      uv = cam_model.project3dToPixel(pt_cv);

      if (uv.x >= 0 && uv.y >= 0 && uv.x < cols && uv.y < rows)
      {

        // Extract espherical coordinates and save max/min
        float range = 0.0;
        float elevation = 0.0;
        float azimuth = 0.0;
        float x = cloud_pcl_orig->points[i].x;
        float y = cloud_pcl_orig->points[i].y;
        float z = cloud_pcl_orig->points[i].z;
        cartesian2SphericalInDegrees(x, y, z, range, azimuth, elevation);
        if (azimuth > 180.0)
        {
          azimuth = azimuth - 360.0;
        }
        if (azimuth > max_azimut)
          max_azimut = azimuth;
        if (azimuth < min_azimut)
          min_azimut = azimuth;
        if (elevation > max_elevation)
          max_elevation = elevation;
        if (elevation < min_elevation)
          min_elevation = elevation;

        //// Save (x, y, z) and (u, v) information
        index[(int)uv.y][(int)uv.x] = i;

        // plot points in image
        static const int RADIUS = 3;
        cv::Mat aux_map(1, 1, CV_8UC3, colorMap(cloud_pcl->points[i].z, factor_color, 255));
        cv::applyColorMap(aux_map, aux_map, cv::COLORMAP_JET);
        float r = aux_map.at < cv::Vec3b > (0, 0)[0];
        float g = aux_map.at < cv::Vec3b > (0, 0)[1];
        float b = aux_map.at < cv::Vec3b > (0, 0)[2];
        cv::circle(plot_image, uv, RADIUS, CV_RGB(r, g, b), -1);
      }
    }
  }

  /****** fill the sensor structure and generate image ******/
  this->sens_config_.max_elevation_angle = max_elevation;
  this->sens_config_.min_elevation_angle = min_elevation;
  this->sens_config_.max_azimuth_angle = max_azimut;
  this->sens_config_.min_azimuth_angle = min_azimut;
  this->sens_config_.grid_azimuth_angular_resolution = 0.2; //// TODO: from parameter
  this->sens_config_.grid_elevation_angular_resolution = 2.0; //// TODO: from parameter
  this->sens_config_.num_of_azimuth_cells = 1
      + (max_azimut - min_azimut) / this->sens_config_.grid_azimuth_angular_resolution;
  this->sens_config_.num_of_elevation_cells = 1
      + (max_elevation - min_elevation) / this->sens_config_.grid_elevation_angular_resolution;
  cv::Mat deph_map_aux(this->sens_config_.num_of_elevation_cells, this->sens_config_.num_of_azimuth_cells, CV_8UC3,
  EMPTY_PIXEL);
  cv::Mat color_map_aux(this->sens_config_.num_of_elevation_cells, this->sens_config_.num_of_azimuth_cells, CV_8UC3,
  EMPTY_PIXEL);
  deph_map_aux.copyTo(depth_map);
  color_map_aux.copyTo(color_map);

  /****** generate deph laser map using spherical representation, and its corresponds image information ******/
  int u = 0, v = 0;
  for (i = 0; i < cols; i++)
  {
    for (j = 0; j < rows; j++)
    {
      if (index[j][i] > NO_INDEX)
      {
        k = index[j][i];
        point2SphericalGrid(cloud_pcl_orig->points[k], this->sens_config_, v, u);
        if (u != INVALID_VALUE)
        {
          depth_map.at < cv::Vec3b > (v, u)[0] = colorMap(cloud_pcl->points[k].z, factor_color, 255);
          depth_map.at < cv::Vec3b > (v, u)[1] = colorMap(cloud_pcl->points[k].z, factor_color, 255);
          depth_map.at < cv::Vec3b > (v, u)[2] = colorMap(cloud_pcl->points[k].z, factor_color, 255);
          color_map.at < cv::Vec3b > (v, u)[0] = last_image.at < cv::Vec3b > (j, i)[2];
          color_map.at < cv::Vec3b > (v, u)[1] = last_image.at < cv::Vec3b > (j, i)[1];
          color_map.at < cv::Vec3b > (v, u)[2] = last_image.at < cv::Vec3b > (j, i)[0];
        }
      }
    }
  }
  fillGraps(depth_map);
  fillGraps(color_map);
  cv::applyColorMap(depth_map, depth_map, cv::COLORMAP_JET);

  return;
}

void OnlineCalibrationAlgorithm::featureMatching(cv::Mat& depth_map, cv::Mat& color_map, cv::Mat& image_matches)
{

  /********** sobel filter **********/
  /*cv::Mat depth_map_gray;
  cv::Mat color_map_gray;
  cv::Mat grad;
  cv::Mat grad_x, grad_y;
  cv::Mat abs_grad_x, abs_grad_y;
  int scale = 1;
  int delta = 0;
  int ddepth = CV_16S;

  cv::GaussianBlur(depth_map, depth_map, cv::Size(3, 3), 0, 0, cv::BORDER_DEFAULT);
  cv::cvtColor(depth_map, depth_map_gray, CV_BGR2GRAY);
  cv::Sobel(depth_map_gray, grad_x, ddepth, 1, 0, 3, scale, delta, cv::BORDER_DEFAULT);
  cv::Sobel(depth_map_gray, grad_y, ddepth, 0, 1, 3, scale, delta, cv::BORDER_DEFAULT);
  convertScaleAbs(grad_x, abs_grad_x);
  convertScaleAbs(grad_y, abs_grad_y);
  addWeighted(abs_grad_x, 0.5, abs_grad_y, 0.5, 0, grad);
  grad.copyTo(depth_map);

  cv::GaussianBlur(color_map, color_map, cv::Size(3, 3), 0, 0, cv::BORDER_DEFAULT);
  cv::cvtColor(color_map, color_map_gray, CV_BGR2GRAY);
  cv::Sobel(color_map_gray, grad_x, ddepth, 1, 0, 3, scale, delta, cv::BORDER_DEFAULT);
  cv::Sobel(color_map_gray, grad_y, ddepth, 0, 1, 3, scale, delta, cv::BORDER_DEFAULT);
  convertScaleAbs(grad_x, abs_grad_x);
  convertScaleAbs(grad_y, abs_grad_y);
  addWeighted(abs_grad_x, 0.5, abs_grad_y, 0.5, 0, grad);
  grad.copyTo(color_map);*/
  /**********************************/

  /********** match templates to get errors between images **********/
  /*cv::Mat result;
  int rows_templates = depth_map.rows / 2;
  int cols_templates = depth_map.cols / 5;
  int result_rows = depth_map.rows - rows_templates + 1;
  int result_cols = depth_map.cols - cols_templates + 1;
  result.create(result_rows, result_cols, CV_32FC1);

  // template 1
  int row_t = rows_templates / 2, col_t = cols_templates / 2;
  cv::Rect roi(col_t, row_t, col_t + cols_templates, row_t + rows_templates);
  cv::Mat template_1 = depth_map(roi);

  cv::matchTemplate(color_map, template_1, result, CV_TM_CCORR);
  cv::normalize(result, result, 0, 1, cv::NORM_MINMAX, -1, cv::Mat());
  result.copyTo(image_matches);*/
  /******************************************************************/


  /********** extract orb features **********/
  std::vector<cv::KeyPoint> kp_depth, kp_color;
  cv::Mat des_depth, des_color;

  // Detect ORB features and compute descriptors.
  cv::Ptr < cv::Feature2D > orb = cv::ORB::create(MAX_FEATURES);
  orb->detectAndCompute(depth_map, cv::Mat(), kp_depth, des_depth);
  orb->detectAndCompute(color_map, cv::Mat(), kp_color, des_color);

  // Match features.
  std::vector < cv::DMatch > matches;
  cv::Ptr < cv::DescriptorMatcher > matcher = cv::DescriptorMatcher::create("BruteForce-Hamming");
  matcher->match(des_depth, des_color, matches, cv::Mat());

  // Remove not so good matches
  const int num_good_matches = GOOD_MATCH_NUM;
  if (matches.size() > num_good_matches)
    matches.erase(matches.begin() + num_good_matches, matches.end());

  // Draw top matches
  cv::drawMatches(depth_map, kp_depth, color_map, kp_color, matches, image_matches);
  /******************************************/

  //debug
  //ROS_INFO("matches size: %d", matches.size());
  //ROS_INFO("keypoints depth size: %d", kp_depth.size());
  //ROS_INFO("keypoints color size: %d", kp_color.size());

  return;
}

////////////////////////////////////////////////////////////////////////////
//////////// Auxiliar functions
void cartesian2SphericalInDegrees(float x, float y, float z, float& range, float& azimuth, float& elevation)
{
  range = sqrt((x * x) + (y * y) + (z * z));

  azimuth = atan2(y, x) * 180.0 / M_PI;
  elevation = atan2(sqrt((x * x) + (y * y)), z) * 180.0 / M_PI;

  if (azimuth < 0)
    azimuth += 360.0;
  if (azimuth >= 360)
    azimuth -= 360;

  if (elevation < 0)
    elevation += 360.0;
  if (elevation >= 360)
    elevation -= 360;

  return;
}

void point2SphericalGrid(pcl::PointXYZI point, struct SensorConfiguration lidar_configuration, int& row, int& col)
{
  float range = 0.0;
  float elevation = 0.0;
  float azimuth = 0.0;

  cartesian2SphericalInDegrees(point.x, point.y, point.z, range, azimuth, elevation);

  if (azimuth > 180.0)
  {
    azimuth = azimuth - 360.0;
  }
  if (azimuth <= lidar_configuration.max_azimuth_angle && azimuth >= lidar_configuration.min_azimuth_angle
      && elevation <= lidar_configuration.max_elevation_angle && elevation >= lidar_configuration.min_elevation_angle)
  {
    col = (int)round(
        (azimuth - lidar_configuration.min_azimuth_angle) / lidar_configuration.grid_azimuth_angular_resolution);
    row = (int)round(
        (elevation - lidar_configuration.min_elevation_angle) / lidar_configuration.grid_elevation_angular_resolution);

    if (col < 0 || col >= lidar_configuration.num_of_azimuth_cells || row < 0
        || row >= lidar_configuration.num_of_elevation_cells)
    {
      row = col = INVALID_VALUE;
    }
  }
}

void fillGraps(cv::Mat& image)
{
  int rows = image.rows;
  int cols = image.cols;
  int i, j, n, m, num_mask;
  float sum_r, sum_g, sum_b, pixel_value;

  for (j = 0; j < rows; j++)
  {
    for (i = 0; i < cols; i++)
    {
      if (image.at < cv::Vec3b > (j, i)[0] == EMPTY_PIXEL)
      {
        num_mask = 0;
        sum_r = 0.0;
        sum_g = 0.0;
        sum_b = 0.0;
        for (m = j - 1; m <= j + 1; m++)
        {
          for (n = i - 1; n <= i + 1; n++)
          {
            if (m >= 0 && m < rows && n >= 0 && n < cols)
            {
              if (image.at < cv::Vec3b > (m, n)[0] > EMPTY_PIXEL)
              {
                num_mask++;
                sum_r = sum_r + image.at < cv::Vec3b > (m, n)[0];
                sum_g = sum_g + image.at < cv::Vec3b > (m, n)[1];
                sum_b = sum_b + image.at < cv::Vec3b > (m, n)[2];
              }
            }
          }
        }
        image.at < cv::Vec3b > (j, i)[0] = sum_r / num_mask;
        image.at < cv::Vec3b > (j, i)[1] = sum_g / num_mask;
        image.at < cv::Vec3b > (j, i)[2] = sum_b / num_mask;
      }
    }
  }
  return;
}

float colorMap(float dist, float factor, int base)
{
  float color_map;

  color_map = (dist / factor) * base;

  if (color_map > base)
  {
    color_map = base;
  }

  return color_map;
}

/*
 * camera_lidar_fuser.cpp
 *
 *  Created on: Jul 18, 2017
 *      Author: ne0
 */

#include "ros/ros.h"
#include <sensor_msgs/point_cloud_conversion.h>
#include <sensor_msgs/PointCloud.h>
#include <sensor_msgs/PointCloud2.h>
#include <sensor_msgs/CameraInfo.h>
#include <sensor_msgs/Image.h>
#include <sensor_msgs/image_encodings.h>

#include <message_filters/subscriber.h>
#include <message_filters/synchronizer.h>
#include <message_filters/time_synchronizer.h>
#include <message_filters/sync_policies/approximate_time.h>

#include <autoware_msgs/CloudCluster.h>
#include <autoware_msgs/CloudClusterArray.h>
#include <autoware_msgs/DetectedObject.h>
#include <autoware_msgs/DetectedObjectArray.h>
#include <autoware_msgs/projection_matrix.h>
#include <autoware_msgs/image_obj.h>

#include <jsk_recognition_msgs/BoundingBox.h>
#include <jsk_recognition_msgs/BoundingBoxArray.h>
#include <jsk_rviz_plugins/Pictogram.h>
#include <jsk_rviz_plugins/PictogramArray.h>

#include <opencv2/opencv.hpp>

#include <cv_bridge/cv_bridge.h>


//TODO:
//1. Sync:
//    -Clusters
//    -Image Detections
//    -Image Frame
//    -Extrinsic Calibration


class CameraLidarFuser
{
private:

	ros::NodeHandle* 	node_handle_;
	ros::Publisher 		cloud_clusters_class_pub_;
	ros::Publisher 		bounding_box_class_pub_;

	ros::Subscriber 	extrinsic_sub_;
	ros::Subscriber 	intrinsic_sub_;

	cv::Mat 			camera_extrinsic_matrix_;
	cv::Mat 			camera_intrinsic_matrix_;
	cv::Mat 			camera_distortion_coeff_;

	cv::Mat				rotation_matrix_;
	cv::Mat				translation_matrix_;

	cv::Size 			camera_image_size;

	typedef message_filters::sync_policies::ApproximateTime<autoware_msgs::CloudClusterArray,
															autoware_msgs::image_obj,
															autoware_msgs::image_obj,
															sensor_msgs::Image> FusionSyncPolicy;

	message_filters::Subscriber<autoware_msgs::CloudClusterArray> cloud_clusters_sub_;
	message_filters::Subscriber<autoware_msgs::image_obj> car_detection_sub_;
	message_filters::Subscriber<autoware_msgs::image_obj> person_detection_sub_;
	message_filters::Subscriber<sensor_msgs::Image> image_sub_;

	message_filters::Synchronizer<FusionSyncPolicy> fusion_sync_;

	bool project_point(const cv::Point3d& in_point_3d,
							cv::Point_<int>& out_point_2d)
	{
		cv::Mat rotation_matrix = camera_extrinsic_matrix_(cv::Rect(0,0,3,3)).t();
		cv::Mat translation_matrix = -rotation_matrix*(camera_extrinsic_matrix_(cv::Rect(3,0,1,3)));

		cv::Mat projected_point(1, 3, CV_64F);
		projected_point.at<double>(0) = double(in_point_3d.x);
		projected_point.at<double>(1) = double(in_point_3d.y);
		projected_point.at<double>(2) = double(in_point_3d.z);
		projected_point = projected_point * rotation_matrix.t() + translation_matrix.t();

		if (projected_point.at<double>(2) <= 2.5) {
			return false;
		}

		double perspective_x = projected_point.at<double>(0) / projected_point.at<double>(2);
		double perspective_y = projected_point.at<double>(1)/projected_point.at<double>(2);
		double r2 = perspective_x * perspective_x + perspective_y * perspective_y;
		double tmp_dist = 1 + camera_distortion_coeff_.at<double>(0) * r2
			+ camera_distortion_coeff_.at<double>(1) * r2 * r2
			+ camera_distortion_coeff_.at<double>(4) * r2 * r2 * r2;

		cv::Point2d imagepoint;
		imagepoint.x = perspective_x * tmp_dist
			+ 2 * camera_distortion_coeff_.at<double>(2) * perspective_x * perspective_y
			+ camera_distortion_coeff_.at<double>(3) * (r2 + 2 * perspective_x * perspective_x);
		imagepoint.y = perspective_y * tmp_dist
			+ camera_distortion_coeff_.at<double>(2) * (r2 + 2 * perspective_y * perspective_y)
			+ 2 * camera_distortion_coeff_.at<double>(3) * perspective_x * perspective_y;
		imagepoint.x = camera_intrinsic_matrix_.at<double>(0,0) * imagepoint.x + camera_intrinsic_matrix_.at<double>(0,2);
		imagepoint.y = camera_intrinsic_matrix_.at<double>(1,1) * imagepoint.y + camera_intrinsic_matrix_.at<double>(1,2);

		int image_px = int(imagepoint.x + 0.5);
		int image_py = int(imagepoint.y + 0.5);
		if(image_px >= 0
			&& image_px < camera_image_size.width
			&& image_py >= 0
			&& image_py < camera_image_size.height)
		{
			out_point_2d.x = image_px;
			out_point_2d.y = image_py;
			return true;
		}
		return false;

	}

	void project_points(const std::vector<cv::Point3d>& in_points_3d,
						std::vector< cv::Point_<int> >& out_points_2d)
	{

		out_points_2d.clear();
		out_points_2d.resize(in_points_3d.size(), cv::Point2d(-1, -1));

		for (std::size_t i=0; i< in_points_3d.size(); i++)
		{
			cv::Point_<int> projected_point;
			if(project_point(in_points_3d[i], projected_point))
			{
				out_points_2d[i] = projected_point;
			}
		}
	}

public:
	void SyncedCallback(const autoware_msgs::CloudClusterArray::ConstPtr& in_lidar_detections,
							const autoware_msgs::image_obj::ConstPtr& in_car_image_detections,
							const autoware_msgs::image_obj::ConstPtr& in_person_image_detections,
							const sensor_msgs::Image::ConstPtr& in_image_raw)
	{
		if (camera_extrinsic_matrix_.empty() || camera_intrinsic_matrix_.empty() || camera_distortion_coeff_.empty()
				|| camera_image_size.height == 0 || camera_image_size.width == 0
			)
		{
			ROS_INFO("Looks like /camera/camera_info and/or /projection_matrix are not being published.. "
					"Check that both are being published..");
			return;
		}

		cv_bridge::CvImagePtr cv_image = cv_bridge::toCvCopy(in_image_raw, sensor_msgs::image_encodings::BGR8);
		cv::Mat image_raw_mat = cv_image->image;

		//cv::Mat tmp(camera_image_size.height, camera_image_size.width, CV_8UC3, cv::Scalar(0,0,0));
		cv::Mat tmp = image_raw_mat.clone();

		for (std::size_t i = 0; i < in_lidar_detections->clusters.size(); i++)
		{
			std::vector< cv::Point_<int> > projected_points;
			std::vector<cv::Point3d> bounding_box_face;
			autoware_msgs::CloudCluster current_cluster = in_lidar_detections->clusters[i];

			cv::Point_<int> min_point, max_point;
			bool min_ok, max_ok;

			min_ok = project_point(cv::Point3d(current_cluster.min_point.point.x,
										current_cluster.min_point.point.y,
										current_cluster.min_point.point.z),
							min_point);

			max_ok = project_point(cv::Point3d(current_cluster.max_point.point.x,
										current_cluster.max_point.point.y,
										current_cluster.max_point.point.z),
							max_point);

			if (min_ok && max_ok
					&& min_point != max_point)
			{
				cv::rectangle(tmp, min_point, max_point, cv::Scalar(0,0,255), 2);
				//cv::circle(tmp, min_point, 2, cv::Scalar(255,0,0));
				//cv::circle(tmp, max_point, 2, cv::Scalar(0,255,0));
			}

			/*bounding_box_face.push_back(cv::Point3d(current_cluster.min_point.point.x,
													current_cluster.min_point.point.y,
													current_cluster.min_point.point.z));
			bounding_box_face.push_back(cv::Point3d(current_cluster.max_point.point.x,
													current_cluster.max_point.point.y,
													current_cluster.max_point.point.z));

			project_points(bounding_box_face, projected_points);*/

		}
		cv::imshow("projections", tmp);
		cv::waitKey(10);
	}

	void ExtrinsicMatrixCallback(const autoware_msgs::projection_matrix& in_projection_matrix)
	{
		camera_extrinsic_matrix_ = cv::Mat(4, 4, CV_64F);
		for (int row=0; row<4; row++)
		{
			for (int col=0; col<4; col++)
			{
				camera_extrinsic_matrix_.at<double>(row, col) = in_projection_matrix.projection_matrix[row * 4 + col];
			}
		}
		//extract rotation and translation matrices from extrinsic
		rotation_matrix_ = camera_extrinsic_matrix_(cv::Rect(0,0,3,3));
		std::cout << rotation_matrix_ << std::endl;
		translation_matrix_ = -rotation_matrix_.t()*(camera_extrinsic_matrix_(cv::Rect(3,0,1,3)));
		translation_matrix_ = translation_matrix_.t();
		std::cout << translation_matrix_ << std::endl;
		ROS_INFO("CameraLidarFuser: Projection Matrix.. OK");
	}

	void IntrinsicMatrixCallback(const sensor_msgs::CameraInfo& in_camera_info)
	{
		camera_image_size.height = in_camera_info.height;
		camera_image_size.width = in_camera_info.width;

		camera_intrinsic_matrix_ = cv::Mat(3, 3, CV_64F);
		for (int row=0; row<3; row++)
		{
			for (int col=0; col<3; col++)
			{
				camera_intrinsic_matrix_.at<double>(row, col) = in_camera_info.K[row * 3 + col];
			}
		}
		camera_distortion_coeff_ = cv::Mat(1, 5, CV_64F);
		for (int col=0; col<5; col++)
		{
			camera_distortion_coeff_.at<double>(col) = in_camera_info.D[col];
		}
		ROS_INFO("CameraLidarFuser: Calibration Matrix.. OK");
	}

	CameraLidarFuser(ros::NodeHandle* in_handle) :
		node_handle_(in_handle),
		cloud_clusters_sub_(*node_handle_, "cloud_clusters" , 1),
		car_detection_sub_(*node_handle_, "obj_car/image_obj" , 1),
		person_detection_sub_(*node_handle_, "obj_person/image_obj" , 1),
		image_sub_(*node_handle_, "image_raw" , 1),
		fusion_sync_(FusionSyncPolicy(10), cloud_clusters_sub_, car_detection_sub_, person_detection_sub_, image_sub_)
	{

		fusion_sync_.registerCallback(boost::bind(&CameraLidarFuser::SyncedCallback, this, _1, _2, _3, _4));

		extrinsic_sub_ = in_handle->subscribe("projection_matrix", 1, &CameraLidarFuser::ExtrinsicMatrixCallback, this);
		intrinsic_sub_ = in_handle->subscribe("camera/camera_info", 1, &CameraLidarFuser::IntrinsicMatrixCallback, this);

		cloud_clusters_class_pub_ = node_handle_->advertise<autoware_msgs::CloudClusterArray>("/cloud_clusters_class", 1);
		bounding_box_class_pub_ = node_handle_->advertise<jsk_recognition_msgs::BoundingBoxArray>("/bounding_box_class", 1);
	}
};

int main(int argc, char **argv)
{
	ros::init(argc, argv, "camera_lidar_fuser");

	ros::NodeHandle node_handle;
	ros::NodeHandle private_node_handle("~");//to receive args

	CameraLidarFuser fusion_node(&node_handle);

	ros::Rate loop_rate(10);
	ROS_INFO("camera_lidar_fuser: Waiting for /image_raw, /cloud_clusters, /obj_car/image_obj, /obj_person/image_obj, "
			"/camera/camera_info and /projection_matrix ");
	while(ros::ok())
	{
		ros::spinOnce();
		loop_rate.sleep();
	}
	return 0;
}

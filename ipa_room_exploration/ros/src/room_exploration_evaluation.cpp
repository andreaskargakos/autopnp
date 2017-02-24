#include <ros/ros.h>
#include <ros/package.h>

#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <iostream>
#include <fstream>
#include <numeric>

#include <opencv/cv.h>
#include <opencv/highgui.h>

#include <cv_bridge/cv_bridge.h>

#include <actionlib/client/simple_action_client.h>
#include <actionlib/client/terminal_state.h>
#include <geometry_msgs/Pose2D.h>
#include <geometry_msgs/Polygon.h>
#include <geometry_msgs/Point32.h>
#include <nav_msgs/OccupancyGrid.h>

#include <ipa_building_msgs/MapSegmentationAction.h>
#include <ipa_building_msgs/RoomExplorationAction.h>
#include <cob_map_accessibility_analysis/CheckPerimeterAccessibility.h>

#include <ipa_room_exploration/dynamic_reconfigure_client.h>
#include <ipa_building_msgs/checkCoverage.h>
#include <ipa_room_exploration/A_star_pathplanner.h>

#include <time.h>
#include <sys/time.h>
#include <ipa_room_exploration/timer.h>

#include <Eigen/Dense>

#include <boost/regex.hpp>

#define PI 3.14159265359

// Overload of << operator for geometry_msgs::Pose2D to wanted format.
std::ostream& operator<<(std::ostream& os, const geometry_msgs::Pose2D& obj)
{
	std::stringstream ss;
	ss <<  "[" << obj.x << ", " << obj.y << ", " << obj.theta << "]";
	os << ss.rdbuf();
	return os;
}


// Struct used to define, which segmentation algorithm together with which exploration algorithm should be used. Also a function is
// provided, that returns a string showing the current configuration --> used to save the results.
struct ExplorationConfig
{
	int exploration_algorithm_;	// this variable selects, which exploration algorithm should be used
									// 1: grid point explorator
									// 2: boustrophedon explorator
									// 3: neural network explorator
									// 4: convexSPP explorator
									// 5: flowNetwork explorator
									// 6: energyFunctional explorator
									// 7: Voronoi explorator

	// default values --> best ones?
	ExplorationConfig()
	{
		exploration_algorithm_ = 2;
	}

	// create one configuration
	ExplorationConfig(const int exploration_algorithm)
	{
		exploration_algorithm_ = exploration_algorithm;
	}

	// function that returns the current configuration as string
	std::string generateConfigurationFolderString() const
	{
		std::stringstream ss;
		ss << "expl" << exploration_algorithm_;
		return ss.str();
	}

	// function that returns the name of the chosen exploration algorithm
	std::string roomExplorationAlgorithmToString() const
	{
		std::string s = "";
		if (exploration_algorithm_ == 1)
			s = "grid point exploration";
		else if (exploration_algorithm_ == 2)
			s = "boustrophedon exploration";
		else if (exploration_algorithm_ == 3)
			s = "neural network exploration";
		else if (exploration_algorithm_ == 4)
			s = "convex SPP exploration";
		else if (exploration_algorithm_ == 5)
			s = "flow network exploration";
		else if (exploration_algorithm_ == 6)
			s = "energy functional exploration";

		return s;
	}
};

// Struct that carries several parameters for the action servers
struct ExplorationData
{
	std::string map_name_;		// without file type
	cv::Mat floor_plan_;
	std::vector<cv::Mat> room_maps_;
	std::vector<cv::Rect> bounding_boxes_;
	float map_resolution_;	// [m/pixel]
	geometry_msgs::Pose map_origin_;
	geometry_msgs::Pose2D robot_start_position_;
	double robot_radius_;
	double robot_speed_; // [m/s]
	double rotation_speed_; // [rad/s]
	std::vector<geometry_msgs::Point32> fow_points_;

	// empty values as default
	ExplorationData()
	{
		map_name_ = "";
		floor_plan_ = cv::Mat();
		map_resolution_ = 0.05;
		map_origin_.position.x = 0;
		map_origin_.position.y = 0;
		robot_radius_ = 0.8;
		robot_speed_ = 0.3;
		rotation_speed_ = 0.1;
	}

	// set data used in this evaluation
	ExplorationData(const std::string map_name, const cv::Mat floor_plan, const float map_resolution, const double robot_radius,
			const std::vector<geometry_msgs::Point32>& fow_points)
	{
		map_name_ = map_name;
		floor_plan_ = floor_plan;
		map_resolution_ = map_resolution;
		robot_radius_ = robot_radius;
		robot_speed_ = 0.3;
		rotation_speed_ = 0.1;
		fow_points_ = fow_points;
		map_origin_.position.x = 0;
		map_origin_.position.y = 0;
		cv::Mat map_eroded;
		cv::erode(floor_plan_, map_eroded, cv::Mat(), cv::Point(-1,-1), robot_radius_/map_resolution_+2);
		cv::Mat distance_map;	//variable for the distance-transformed map, type: CV_32FC1
		cv::distanceTransform(map_eroded, distance_map, CV_DIST_L2, 5);
		cv::convertScaleAbs(distance_map, distance_map);	// conversion to 8 bit image
		bool robot_start_coordinate_set = false;
		for (int v=0; v<map_eroded.rows && robot_start_coordinate_set==false; ++v)
			for (int u=0; u<map_eroded.cols && robot_start_coordinate_set==false; ++u)
				if (map_eroded.at<uchar>(v,u) != 0 && distance_map.at<uchar>(v,u) > 20)			// TODO: parameter
				{
					robot_start_position_.x = u*map_resolution_ + map_origin_.position.x;
					robot_start_position_.y = v*map_resolution_ + map_origin_.position.y;
					robot_start_coordinate_set = true;
				}
	}
};

struct EvaluationResults
{
	double calculation_time;
	double path_length;
	double execution_time;
	cv::Mat room_map, coverage_map;
	int number_of_turns;
};

// class that segments the wanted maps, finds for each resulting room a coverage path and saves these paths
class ExplorationEvaluation
{
protected:

	// function that creates configurations to get all possible combinations of segmentations and exploration algorithms
	void setConfigurations(std::vector<ExplorationConfig>& configurations, const std::vector<int>& exploration_algorithms)
	{
		for(std::vector<int>::const_iterator expl=exploration_algorithms.begin(); expl!=exploration_algorithms.end(); ++expl)
		{
			ExplorationConfig current_config(*expl);
			configurations.push_back(current_config);
		}
	}

public:

	ros::NodeHandle node_handle_;


	ExplorationEvaluation(ros::NodeHandle& nh, const std::string& test_map_path, const std::string& data_storage_path,
			const double robot_radius, const std::vector<int>& exploration_algorithms, const std::vector<geometry_msgs::Point32>& fow_points)
	{
		// set node-handle
		node_handle_ = nh;

		// prepare relevant floor map data
		std::vector< std::string > map_names;
		map_names.push_back("lab_ipa");
//		map_names.push_back("lab_c_scan");
//		map_names.push_back("Freiburg52_scan");
//		map_names.push_back("Freiburg79_scan");
//		map_names.push_back("lab_b_scan");
//		map_names.push_back("lab_intel");
//		map_names.push_back("Freiburg101_scan");
//		map_names.push_back("lab_d_scan");
//		map_names.push_back("lab_f_scan");
//		map_names.push_back("lab_a_scan");
//		map_names.push_back("NLB");
//		map_names.push_back("office_a");
//		map_names.push_back("office_b");
//		map_names.push_back("office_c");
//		map_names.push_back("office_d");
//		map_names.push_back("office_e");
//		map_names.push_back("office_f");
//		map_names.push_back("office_g");
//		map_names.push_back("office_h");
//		map_names.push_back("office_i");
//		map_names.push_back("lab_ipa_furnitures");
//		map_names.push_back("lab_c_scan_furnitures");
//		map_names.push_back("Freiburg52_scan_furnitures");
//		map_names.push_back("Freiburg79_scan_furnitures");
//		map_names.push_back("lab_b_scan_furnitures");
//		map_names.push_back("lab_intel_furnitures");
//		map_names.push_back("Freiburg101_scan_furnitures");
//		map_names.push_back("lab_d_scan_furnitures");
//		map_names.push_back("lab_f_scan_furnitures");
//		map_names.push_back("lab_a_scan_furnitures");
//		map_names.push_back("NLB_furnitures");
//		map_names.push_back("office_a_furnitures");
//		map_names.push_back("office_b_furnitures");
//		map_names.push_back("office_c_furnitures");
//		map_names.push_back("office_d_furnitures");
//		map_names.push_back("office_e_furnitures");
//		map_names.push_back("office_f_furnitures");
//		map_names.push_back("office_g_furnitures");
//		map_names.push_back("office_h_furnitures");
//		map_names.push_back("office_i_furnitures");

		// create all needed configurations
		std::vector<ExplorationConfig> configs;
		setConfigurations(configs, exploration_algorithms);

		// prepare images and evaluation datas
		std::vector<ExplorationData> evaluation_datas;
		for (size_t image_index = 0; image_index<map_names.size(); ++image_index)
		{
			std::string image_filename = test_map_path + map_names[image_index] + ".png";
			std::cout << "loading image: " << image_filename << std::endl;
			cv::Mat map = cv::imread(image_filename.c_str(), 0);
			//make non-white pixels black
			for (int y=0; y<map.rows; y++)
			{
				for (int x=0; x<map.cols; x++)
				{
					if (map.at<unsigned char>(y, x)>250)
					{
						map.at<unsigned char>(y, x)=255;
					}
					else //if (map.at<unsigned char>(y, x) != 255)
					{
						map.at<unsigned char>(y, x)=0;
					}
				}
			}

			// create evaluation data
			evaluation_datas.push_back(ExplorationData(map_names[image_index], map, 0.05, robot_radius, fow_points));
		}

		// get the room maps for each evaluation data
		getRoomMaps(evaluation_datas);

		// compute exploration paths for each room in the maps
		std::string bugfile = data_storage_path + "bugfile.txt";
		std::ofstream failed_maps(bugfile.c_str(), std::ios::out);
		if (failed_maps.is_open())
			failed_maps << "Maps that had a bug during the simulation and couldn't be finished: " << std::endl;
		ROS_INFO("Evaluating the maps.");
		for (size_t i=0; i<evaluation_datas.size(); ++i)
		{
			if (planCoveragePaths(configs, evaluation_datas[i], data_storage_path)==false)
			{
				std::cout << "failed to simulate map " << evaluation_datas[i].map_name_ << std::endl;
				if (failed_maps.is_open())
					failed_maps << evaluation_datas[i].map_name_ << std::endl;
			}
		}
		if (failed_maps.is_open())
			failed_maps.close();

		// read out the computed paths and calculate the evaluation values
		ROS_INFO("Reading out all saved paths.");
		// TODO: finish
		std::vector<EvaluationResults> results;
		for (size_t i=0; i<evaluation_datas.size(); ++i)
		{
			evaluateCoveragePaths(configs, evaluation_datas[i], results, data_storage_path);
		}

	}

	void getRoomMaps(std::vector<ExplorationData>& data_saver)
	{
		for(std::vector<ExplorationData>::iterator datas=data_saver.begin(); datas!=data_saver.end(); ++datas)
		{
			// 1. read out the ground truth map
			std::string map_name_basic = datas->map_name_;
			std::size_t pos = datas->map_name_.find("_furnitures");
			if (pos != std::string::npos)
				map_name_basic = datas->map_name_.substr(0, pos);
			std::string gt_image_filename = ros::package::getPath("ipa_room_segmentation") + "/common/files/test_maps/" + map_name_basic + "_gt_segmentation.png";
			std::cout << "Loading ground truth segmentation from: " << gt_image_filename << std::endl;
			cv::Mat gt_map = cv::imread(gt_image_filename.c_str(), CV_8U);
			cv::threshold(gt_map, gt_map, 250, 255, CV_THRESH_BINARY);

			// 2. retrieve the rooms for each ground truth map and get the maps that show only one room each
			int label = 1;
			std::vector<cv::Rect> bounding_boxes;
			cv::Mat labeled_map;
			gt_map.convertTo(labeled_map, CV_32SC1);
			for (int y = 0; y < gt_map.rows; y++)
			{
				for (int x = 0; x < gt_map.cols; x++)
				{
					if (gt_map.at<uchar>(y,x)!=255 || labeled_map.at<int>(y,x)!=255)
						continue;

					// fill each room area with a unique id
					cv::Rect rect;
					cv::floodFill(labeled_map, cv::Point(x,y), label, &rect, 0, 0, 8);

					// save the bounding box to retrieve the min/max coordinates
					bounding_boxes.push_back(rect);

					++label;
				}
			}
			std::vector<cv::Mat> room_maps;
			std::vector<cv::Rect> chosen_bb;
			for(int room=1; room<label; ++room)
			{
				cv::Mat room_map = cv::Mat(labeled_map.rows, labeled_map.cols, CV_8U, cv::Scalar(0));
				// go trough pixels and make pixels belonging to room white and not belonging pixels black
				for(size_t y=0; y<room_map.rows; ++y)
					for(size_t x=0; x<room_map.cols; ++x)
						if(labeled_map.at<int>(y,x)==room)
							room_map.at<uchar>(y,x) = 255;

				// check for the eroded map (the map that shows the in reality reachable areas) to have enough free pixels
				cv::Mat eroded_map;
				int robot_radius_in_pixel = (datas->robot_radius_ / datas->map_resolution_);
				cv::erode(room_map, eroded_map, cv::Mat(), cv::Point(-1, -1), robot_radius_in_pixel);
				int number_of_pixels = 0;
				for(size_t y=0; y<eroded_map.rows; ++y)
					for(size_t x=0; x<eroded_map.cols; ++x)
						if(eroded_map.at<uchar>(y,x)==255)
							++number_of_pixels;

				// save room map, if region is big enough
				if(number_of_pixels>0)
				{
					room_maps.push_back(room_map);
					chosen_bb.push_back(bounding_boxes[room-1]);
//					cv::rectangle(room_map, bounding_boxes[room-1], cv::Scalar(127), 2);
//					cv::imshow("room", room_map);
//					cv::waitKey();
				}
			}

			// save the found room maps and bounding boxes
			datas->floor_plan_ = gt_map;
			datas->room_maps_ = room_maps;
			datas->bounding_boxes_ = chosen_bb;
		}
	}

	// function that does the whole evaluation for all configs
	bool planCoveragePaths(const std::vector<ExplorationConfig>& configs, ExplorationData& datas, const std::string data_storage_path)
	{
		// go trough all configs and do the evaluations
		for(std::vector<ExplorationConfig>::const_iterator config=configs.begin(); config!=configs.end(); ++config)
		{
			// create a folder for the log directory
			std::string folder_path = config->generateConfigurationFolderString() + "/";
			const std::string upper_command = "mkdir -p " + data_storage_path + folder_path;
			int return_value = system(upper_command.c_str());

			std::cout << "expl: " << config->exploration_algorithm_ << std::endl;
			//variables for time measurement
			struct timespec t0, t1;

			// go trough all rooms and find the coverage path trough it
			geometry_msgs::Polygon region_of_interest; // always assume full map in this case
			geometry_msgs::Point32 edge_point;
			edge_point.x = 0;
			edge_point.y = 0;
			region_of_interest.points.push_back(edge_point);
			edge_point.x = datas.floor_plan_.cols;
			edge_point.y = datas.floor_plan_.rows;
			region_of_interest.points.push_back(edge_point);
			std::stringstream output;
			for(size_t room_index=0; room_index<datas.room_maps_.size(); ++room_index)
			{
				cv::Mat room_map = datas.room_maps_[room_index];

				// find min/max coordinates for this room by using the saved bounding box
//				int min_y = 1e5, max_y = 0, min_x = 1e5, max_x = 0;
//				for (int y = 0; y < room_map.rows; y++)
//				{
//					for (int x = 0; x < room_map.cols; x++)
//					{
//						//only check white pixels
//						if(room_map.at<uchar>(y,x)==255)
//						{
//							if(y < min_y)
//								min_y = y;
//							if(y > max_y)
//								max_y = y;
//							if(x < min_x)
//								min_x = x;
//							if(x > max_x)
//								max_x = x;
//						}
//					}
//				}
				int min_y = datas.bounding_boxes_[room_index].y;
				int max_y = datas.bounding_boxes_[room_index].y+datas.bounding_boxes_[room_index].height;
				int min_x = datas.bounding_boxes_[room_index].x;
				int max_x = datas.bounding_boxes_[room_index].x+datas.bounding_boxes_[room_index].width;
				min_y -= 1;
				min_x -= 1;
				max_y += 1;
				max_x += 1;
				std::cout << "min coordinates: " << min_y << ":" << max_y << "(y), " << min_x << ":" << max_x << "(x)" << std::endl;
				geometry_msgs::Polygon min_max_points;
				geometry_msgs::Point32 min_point, max_point;
				min_point.x = min_x;
				min_point.y = min_y;
				max_point.x = max_x;
				max_point.y = max_y;
				min_max_points.points;
				min_max_points.points.push_back(min_point);
				min_max_points.points.push_back(max_point);

				// send the exploration goal
				ipa_building_msgs::RoomExplorationResultConstPtr result_expl;
				clock_gettime(CLOCK_MONOTONIC, &t0); //set time stamp before the path planning
				if(planCoveragePath(room_map, datas, *config, result_expl, min_max_points, region_of_interest)==false)
				{
					output << "room " << room_index << " exceeded the time limitation for computation" << std::endl << std::endl;
					continue;
				}
				clock_gettime(CLOCK_MONOTONIC,  &t1); //set time stamp after the path planning

				// retrieve the solution and save the found results
				double calculation_time = (t1.tv_sec - t0.tv_sec) + (double) (t1.tv_nsec - t0.tv_nsec) * 1e-9;
				std::vector<geometry_msgs::Pose2D> coverage_path = result_expl->coverage_path;
				// transform path to map coordinates
				std::cout << "length of path: " << coverage_path.size() << std::endl;
				if(coverage_path.size()==0)
				{
					output << "room " << room_index << " had a bug" << std::endl << std::endl;
					continue;
				}
				for(size_t point=0; point<coverage_path.size(); ++point)
				{
					coverage_path[point].x = (coverage_path[point].x-datas.map_origin_.position.x)/datas.map_resolution_;
					coverage_path[point].y = (coverage_path[point].y-datas.map_origin_.position.y)/datas.map_resolution_;
				}
				output << "calculation time: " << calculation_time << "s" << std::endl;
				for(size_t point=0; point<coverage_path.size(); ++point)
					output << coverage_path[point] << std::endl;
				output << std::endl;

				// display path
				cv::Mat path_map = room_map.clone();
				for (size_t point=0; point<coverage_path.size(); ++point)
				{
					cv::circle(path_map, cv::Point(coverage_path[point].x, coverage_path[point].y), 2, cv::Scalar(128), -1);
					if (point > 0)
						cv::line(path_map, cv::Point(coverage_path[point].x, coverage_path[point].y), cv::Point(coverage_path[point-1].x, coverage_path[point-1].y), cv::Scalar(128), 1);
				}
				cv::imshow("path", path_map);
				cv::waitKey();
			}
			std::string log_filename = data_storage_path + folder_path + datas.map_name_ + "_results.txt";
			std::cout << log_filename << std::endl;
			std::ofstream file(log_filename.c_str(), std::ios::out);
			if (file.is_open()==true)
				file << output.str();
			else
				ROS_ERROR("Error on writing file '%s'", log_filename.c_str());
			file.close();
		}

		// if all configurations finished, return a boolean showing success
		return true;
	}

	// function that reads out the calculated paths and does the evaluation of the calculated these
	void evaluateCoveragePaths(const std::vector<ExplorationConfig>& configs, const ExplorationData& datas,
			std::vector<EvaluationResults>& results, const std::string data_storage_path)
	{
		// find the middle-point distance of the given field of view
		std::vector<Eigen::Matrix<float, 2, 1> > fow_vectors;
		for(int i = 0; i < 4; ++i)
		{
			Eigen::Matrix<float, 2, 1> current_vector;
			current_vector << datas.fow_points_[i].x, datas.fow_points_[i].y;
			fow_vectors.push_back(current_vector);
		}
		// get the distance to the middle-point
		Eigen::Matrix<float, 2, 1> middle_point = (fow_vectors[0] + fow_vectors[1] + fow_vectors[2] + fow_vectors[3]) / 4;
		double distance_robot_fow_middlepoint = middle_point.norm();

		for(std::vector<ExplorationConfig>::const_iterator config=configs.begin(); config!=configs.end(); ++config)
		{
			// 1. get the location of the results and open this file
			std::string folder_path = config->generateConfigurationFolderString() + "/";
			std::cout << folder_path << datas.map_name_ << std::endl;
			std::string log_filename = data_storage_path + folder_path + datas.map_name_ + "_results.txt";
			std::ifstream reading_file(log_filename.c_str(), std::ios::in);

			// 2. if the file could be opened, read out the given paths for all rooms
			std::vector<std::vector<geometry_msgs::Pose2D> > paths;
			std::vector<double> calculation_times;
			std::vector<geometry_msgs::Pose2D> current_path;
			if (reading_file.is_open()==true)
			{
				std::string line;
				bool initial = true;
				while(getline(reading_file, line))
				{
					double calculation_time;
					double x, y, theta;

					// check if the current line is empty --> shows a new room
					if(line.empty()==true)
					{
						// save the previously found calculation times and paths
						paths.push_back(current_path);

						// reset temporary vectors
						current_path.clear();

						// set the flag to a new room
						initial = true;

						// ignore the empty line
						continue;
					}

					// if the new line is the first after an empty line, it contains the calculation time
					if(initial==true)
					{
						// if the time limit was exceeded or a bug appeared, save a -1
						if(line.find("exceeded")!=std::string::npos || line.find("bug")!=std::string::npos)
						{
//							std::cout << "exceeded calculation time" << std::endl;
							// set max calculation time
							if(config->exploration_algorithm_==5) // higher max time for flowNetwork explorator
								calculation_time = 10800;
							else
								calculation_time = 1800;

							// save a invalid pose, to show that this room has no coverage path
							geometry_msgs::Pose2D false_pose;
							false_pose.x = -1;
							false_pose.y = -1;
							current_path.push_back(false_pose);
						}
						else
						{
							const char* str = line.c_str();
							sscanf(str, "%*[^0-9]%lf", &calculation_time);
//							std::cout << "calculation time: " << calculation_time << "s" << std::endl;
						}
						calculation_times.push_back(calculation_time);
						initial = false;
					}
					// else read out x,y and theta and create a new Pose
					else
					{
						std::istringstream iss(line);
						int pos_counter = 0;
						std::string buffer;

						// get saved output until comma or the end of the line is reached
						while(getline(iss, buffer, ','))
						{
							// remove brackets
							buffer.erase(std::remove(buffer.begin(), buffer.end(), '['), buffer.end());
							buffer.erase(std::remove(buffer.begin(), buffer.end(), ']'), buffer.end());

							// save value regarding the position of it
							if(pos_counter==0)
								x = atof(buffer.c_str());
							else if(pos_counter==1)
								y = atof(buffer.c_str());
							else if(pos_counter==2)
								theta = atof(buffer.c_str());

							// increase position counter
							++pos_counter;

						}
//						std::cout << "x: " << x << ", y: " << y << ", theta: " << theta << std::endl;

						// save the found pose
						if(x>0 && y>0)
						{
							geometry_msgs::Pose2D current_pose;
							current_pose.x = x;
							current_pose.y = y;
							current_pose.theta = theta;
							current_path.push_back(current_pose);
						}
					}
				}
			}
			else
				ROS_WARN("Error on reading file '%s'", log_filename.c_str());
			reading_file.close();

			// 3. do the evaluations
			AStarPlanner path_planner;
			// 3.1 overall, average pathlength and variance of it for the calculated paths and get the numbers of the turns
			cv::Mat map = datas.floor_plan_.clone();
			cv::Mat eroded_map;
			int robot_radius_in_pixel = (datas.robot_radius_ / datas.map_resolution_);
			cv::erode(map, eroded_map, cv::Mat(), cv::Point(-1, -1), robot_radius_in_pixel);
			cv::Mat path_map = eroded_map.clone();
			std::vector<double> pathlengths_for_map;
			std::vector<std::vector<geometry_msgs::Pose2D> > interpolated_paths; // variable that stores the path points and the points between them
			int nonzero_paths = 0;
			std::vector<double> rotation_values;
			std::vector<int> number_of_rotations, numbers_of_crossings;

			// calculate the gradient directions for each pixel in the map
			// generate matrices for gradient in x/y direction
			cv::Mat gradient_x, gradient_y;
			cv::Mat gradient_map = cv::Mat(map.rows, map.cols, CV_64F, cv::Scalar(0));

			// compute gradient in x direction
			cv::Sobel(map, gradient_x, CV_64F, 1, 0, 3, 1.0, 0.0, cv::BORDER_DEFAULT);

			// compute gradient in y direction
			cv::Sobel(map, gradient_y, CV_64F, 0, 1, 3, 1.0, 0.0, cv::BORDER_DEFAULT);

			// compute the direction of the gradient for each pixel and save the occurring gradients
			for(size_t y=0; y<map.rows; ++y)
			{
				for(size_t x=0; x<map.cols; ++x)
				{
					int dx= gradient_x.at<double>(y,x);
					int dy= gradient_y.at<double>(y,x);
					if(dy*dy+dx*dx!=0)
					{
						double current_gradient = std::atan2(dy, dx);
						gradient_map.at<double>(y,x) = current_gradient;
					}
				}
			}
			for(size_t room=0; room<paths.size(); ++room)
			{
//				std::cout << "room " << room << ", size of path: " << paths[room].size() << std::endl;

				if(paths[room].size()==0)
					continue;
				else
					++nonzero_paths;

				double current_pathlength = 0.0;
				std::vector<geometry_msgs::Pose2D> current_pose_path;
				double previous_angle =paths[room].begin()->theta;
				double current_rotation_abs = 0.0;
				int current_number_of_rotations = 0, current_number_of_crossings = 0;
				geometry_msgs::Pose2D robot_position;
				robot_position = *paths[room].begin();
				// initialize path
				geometry_msgs::Pose2D initial_pose;
				initial_pose.x = (robot_position.x*datas.map_resolution_)+datas.map_origin_.position.x;
				initial_pose.y = (robot_position.y*datas.map_resolution_)+datas.map_origin_.position.y;
				initial_pose.theta = robot_position.theta;
				current_pose_path.push_back(initial_pose);
				for(std::vector<geometry_msgs::Pose2D>::iterator pose=paths[room].begin()+1; pose!=paths[room].end(); ++pose)
				{
					// vector that stores the current path from one pose to another
					std::vector<cv::Point> current_interpolated_path;

					// if a false pose has been saved, ignore it
					if(robot_position.x==-1 && robot_position.y==-1)
						continue;

					// find an accessible next pose
					geometry_msgs::Pose2D next_pose;
					bool found_next = false;
					if(map.at<uchar>(pose->y, pose->x)!=0) // if calculated pose is accessible, use it as next pose
					{
						next_pose = *pose;
						found_next = true;
					}
					else // use the map accessibility server to find another accessible pose
					{
						// get the desired fov-position
						geometry_msgs::Pose2D relative_vector;
						relative_vector.x = std::cos(pose->theta)*distance_robot_fow_middlepoint;
						relative_vector.y = std::sin(pose->theta)*distance_robot_fow_middlepoint;
						geometry_msgs::Pose2D center;
						center.x = pose->x + relative_vector.x;
						center.y = pose->y + relative_vector.y;

						// check for another robot pose to reach the desired fow-position
						std::string perimeter_service_name = "/map_accessibility_analysis/map_perimeter_accessibility_check";
						cob_map_accessibility_analysis::CheckPerimeterAccessibility::Response response;
						cob_map_accessibility_analysis::CheckPerimeterAccessibility::Request check_request;
						check_request.center = center;

						check_request.radius = distance_robot_fow_middlepoint;
						check_request.rotational_sampling_step = PI/8;

						// send request
						if(ros::service::call(perimeter_service_name, check_request, response)==true)
						{
							// find one point on this perimeter check that is accessible and set it as new desired pose
							for(std::vector<geometry_msgs::Pose2D>::iterator new_pose=response.accessible_poses_on_perimeter.begin(); new_pose!=response.accessible_poses_on_perimeter.end(); ++new_pose)
							{
								// transform to image coordinates
								geometry_msgs::Pose2D candidate;
								candidate.x = (new_pose->x-datas.map_origin_.position.x)/datas.map_resolution_;
								candidate.y = (new_pose->y-datas.map_origin_.position.y)/datas.map_resolution_;
								candidate.theta = new_pose->theta;
								// check if this pose is accessible
								if(map.at<uchar>(candidate.y, candidate.x)!=0)
								{
									next_pose = candidate;
									found_next = true;
									break;
								}
							}
						}
					}

					// if no accessible position could be found, go to next possible path point
					if(found_next==false)
						continue;

					// get the angle and check if it the same as before, if not add the rotation
					double angle_difference = previous_angle-next_pose.theta;

					if(angle_difference!=0.0)
					{
						++current_number_of_rotations;
						current_rotation_abs += std::abs(angle_difference);
					}
					// save current angle of pose
					previous_angle = next_pose.theta;

					// create output map to show path --> also check if one point has already been visited
					cv::LineIterator line(eroded_map, cv::Point(next_pose.x, next_pose.y), cv::Point(robot_position.x, robot_position.y), 8);
					cv::circle(eroded_map, cv::Point(next_pose.x, next_pose.y), 2, cv::Scalar(100), CV_FILLED);
					for(int pos=1; pos<line.count-1; pos++, ++line)
					{
						cv::Point current_point = line.pos();
						if(eroded_map.at<uchar>(current_point)==127)
							++current_number_of_crossings;
						else
							eroded_map.at<uchar>(current_point)=127;
//						cv::imshow("er", eroded_map);
//						cv::waitKey();
					}
//					cv::line(eroded_map, cv::Point(next_pose.x, next_pose.y), cv::Point(robot_position.x, robot_position.y), cv::Scalar(100));

					// find pathlength and path between two consecutive poses
//					cv::imshow("er", eroded_map);
//					cv::waitKey();
					current_pathlength += path_planner.planPath(map, cv::Point(robot_position.x, robot_position.y), cv::Point(next_pose.x, next_pose.y), 1.0, 0.0, datas.map_resolution_, 0, &current_interpolated_path);

					if(current_interpolated_path.size()==0)
						continue;

					// transform the cv::Point path to geometry_msgs::Pose2D --> last point has, first point was already gone a defined angle
					for(std::vector<cv::Point>::iterator point=current_interpolated_path.begin()+1; point!=current_interpolated_path.end(); ++point)
					{
						// mark in path map
						path_map.at<uchar>(*point)=127;

						// transform to world coordinates
						geometry_msgs::Pose2D current_pose;
						current_pose.x = (point->x*datas.map_resolution_)+datas.map_origin_.position.x;
						current_pose.y = (point->y*datas.map_resolution_)+datas.map_origin_.position.y;

						// if the current point is the last, use the provided angle
						if(point-current_interpolated_path.begin()==current_interpolated_path.size()-1)
							current_pose.theta = (pose+1)->theta;
						else // calculate angle s.t. it points to the next point
							current_pose.theta = std::atan2((point+1)->y-point->y, (point+1)->x-point->x);

						// add the pose to the path
						current_pose_path.push_back(current_pose);
					}

					// set robot_position to new one
					robot_position = next_pose;
				}

				// save number of crossings of the path
				numbers_of_crossings.push_back(current_number_of_crossings);

				// save rotation values
				rotation_values.push_back(current_rotation_abs);
				number_of_rotations.push_back(current_number_of_rotations);

				// save the interpolated path between
				interpolated_paths.push_back(current_pose_path);

				// transform the pixel length to meter
				current_pathlength *= datas.map_resolution_;
//				std::cout << "length: " << current_pathlength << "m" << std::endl;
				pathlengths_for_map.push_back(current_pathlength);
//				cv::imshow("room paths", eroded_map);
//				cv::waitKey();
			}
			std::cout << "got and drawn paths" << std::endl;

			// save the map with the drawn in coverage paths
			std::stringstream map_filename;
			std::string image_path = data_storage_path + folder_path + datas.map_name_ + "_paths.png";
//			std::cout << image_path << std::endl;
			cv::imwrite(image_path.c_str(), eroded_map);
//			cv::imshow("room paths", room_map);
//			cv::waitKey();

			// calculate the overall pathlength, the average and the variance
			double overall_pathlength = std::accumulate(pathlengths_for_map.begin(), pathlengths_for_map.end(), 0.0);
			double average_pathlength = overall_pathlength/nonzero_paths;
			double pathlength_variance_squared = 0;
			std::vector<double> travel_times_in_rooms;
			for(std::vector<double>::iterator length=pathlengths_for_map.begin(); length!=pathlengths_for_map.end(); ++length)
			{
				pathlength_variance_squared += std::pow(*length-average_pathlength, 2.0);
				travel_times_in_rooms.push_back(*length/datas.robot_speed_);
			}
			pathlength_variance_squared /= nonzero_paths;

			// 2. calculate the execution time by using the robot speed and the rotation speed
			double average_execution_time = 0.0;
			double execution_time_squared_variance = 0.0;
			double overall_execution_time = overall_pathlength/datas.robot_speed_; // travel time
			std::vector<double> rotation_times_in_rooms;
			for(std::vector<double>::iterator rotation=rotation_values.begin(); rotation!=rotation_values.end(); ++rotation)
			{
				overall_execution_time += *rotation/datas.rotation_speed_;
				rotation_times_in_rooms.push_back(*rotation/datas.rotation_speed_);
			}
			average_execution_time = overall_execution_time/nonzero_paths;
			// compute variance
			for(size_t room=0; room<paths.size(); ++room)
			{
				if(paths[room].size()==0)
					continue;

				execution_time_squared_variance += std::pow(rotation_times_in_rooms[room]+travel_times_in_rooms[room]-average_execution_time, 2.0);
			}

			// 3. calculate turn specific values
			double number_of_turns_deviation = 0.0, turn_value_deviation = 0.0;
			double average_number_of_turns = std::accumulate(number_of_rotations.begin(), number_of_rotations.end(), 0);
			double average_turn_value = std::accumulate(rotation_values.begin(), rotation_values.end(), 0);
			average_number_of_turns /= number_of_rotations.size();
			average_turn_value /= rotation_values.size();
			for(size_t room=0; room<number_of_rotations.size(); ++room)
			{
				number_of_turns_deviation += std::pow(number_of_rotations[room]-average_number_of_turns, 2.0);
				turn_value_deviation += std::pow(rotation_values[room]-average_turn_value, 2.0);
			}
			number_of_turns_deviation /= number_of_rotations.size();
			turn_value_deviation /= rotation_values.size();

			// 4. coverage percentage and number of covering each pixel when executing the coverage paths
			std::vector<double> area_covered_percentages;
			std::vector<double> numbers_of_coverages;
			std::string coverage_service_name = "/coverage_check_server/coverage_check";

			geometry_msgs::Pose2D origin;
			origin.x = datas.map_origin_.position.x;
			origin.y = datas.map_origin_.position.y;
			size_t path_index = 0;
			for(size_t room=0; room<paths.size(); ++room)
			{
				// ignore paths with size 0
				if(paths[room].size()==0)
					continue;

				// map that has the seen areas drawn in
				cv::Mat seen_positions_map, number_of_coverages_map;

				// use the provided server to check which areas have been seen
				ipa_building_msgs::checkCoverageRequest coverage_request;
				ipa_building_msgs::checkCoverageResponse coverage_response;
				// fill request
				sensor_msgs::ImageConstPtr service_image;
				cv_bridge::CvImage cv_image;
				cv_image.encoding = "mono8";
				cv_image.image = datas.room_maps_[room];
				service_image = cv_image.toImageMsg();
				coverage_request.input_map = *service_image;
				coverage_request.path = interpolated_paths[path_index];
				coverage_request.field_of_view = datas.fow_points_;
				coverage_request.map_origin = origin;
				coverage_request.map_resolution = datas.map_resolution_;
				coverage_request.check_for_footprint = false;
				coverage_request.check_number_of_coverages = true;
				// send request
				if(ros::service::call(coverage_service_name, coverage_request, coverage_response)==true)
				{
					cv_bridge::CvImagePtr cv_ptr_obj;
					cv_ptr_obj = cv_bridge::toCvCopy(coverage_response.coverage_map, sensor_msgs::image_encodings::MONO8);
					seen_positions_map = cv_ptr_obj->image;

					cv_ptr_obj = cv_bridge::toCvCopy(coverage_response.number_of_coverage_image, sensor_msgs::image_encodings::TYPE_32SC1);
					number_of_coverages_map = cv_ptr_obj->image;
				}
				else
				{
					ROS_INFO("Error when calling the coverage check server.");
				}
//				cv::imshow("seen", seen_positions_map);
//				cv::waitKey();

				// get the area of the whole room
				int white_room_pixels = cv::countNonZero(datas.room_maps_[room]);
				double room_area = datas.map_resolution_ * datas.map_resolution_ * (double) white_room_pixels;

				// get the covered area of the room
				cv::threshold(seen_positions_map, seen_positions_map, 150, 255, cv::THRESH_BINARY); // covered area drawn in as 127 --> find still white pixels
				int not_covered_pixels = cv::countNonZero(seen_positions_map);
				double not_covered_area = datas.map_resolution_ * datas.map_resolution_ * (double) not_covered_pixels;

				// get and save the percentage of coverage
				double coverage_percentage = (room_area-not_covered_area)/room_area;
				area_covered_percentages.push_back(coverage_percentage);

				// check how often pixels have been covered
				double average_coverage_number = 0.0, coverage_number_deviation = 0.0;
				for(size_t u=0; u<number_of_coverages_map.rows; ++u)
					for(size_t v=0; v<number_of_coverages_map.cols; ++v)
						if(number_of_coverages_map.at<int>(u,v)!=0)
							numbers_of_coverages.push_back(number_of_coverages_map.at<int>(u,v));

				// increase index of interpolated path
				++path_index;
			}
			std::cout << "checked coverage for all rooms" << std::endl;

			// calculate average coverage and deviation
			double average_coverage_percentage = std::accumulate(area_covered_percentages.begin(), area_covered_percentages.end(), 0.0);
			average_coverage_percentage /= interpolated_paths.size();
			double coverage_deviation = 0.0;
			for(std::vector<double>::iterator perc=area_covered_percentages.begin(); perc!=area_covered_percentages.end(); ++perc)
				coverage_deviation += std::pow(average_coverage_percentage-*perc, 2.0);
			coverage_deviation /= interpolated_paths.size();
			double average_coverage_number = std::accumulate(numbers_of_coverages.begin(), numbers_of_coverages.end(), 0.0);
			average_coverage_number /= numbers_of_coverages.size();
			double coverage_number_deviation = 0.0;
			for(std::vector<double>::iterator cov=numbers_of_coverages.begin(); cov!=numbers_of_coverages.end(); ++cov)
					coverage_number_deviation += std::pow(average_coverage_number-*cov, 2.0);
			coverage_number_deviation /= numbers_of_coverages.size();

			// 5. compute average computation time and deviation
			double average_computation_time = std::accumulate(calculation_times.begin(), calculation_times.end(), 0.0);
			average_computation_time /= calculation_times.size();
			double computation_time_devition = 0.0;
			for(std::vector<double>::iterator tim=calculation_times.begin(); tim!=calculation_times.end(); ++tim)
				computation_time_devition += std::pow(average_computation_time-*tim, 2.0);
			computation_time_devition /= calculation_times.size();

			// 6. for each part of the path calculate the parallelities with respect to the nearest wall and the nearest trajectory part
			std::vector<std::vector<double> > wall_angle_differences, trajectory_angle_differeneces;
			std::vector<std::vector<int> > revisit_times; // vector that stores the index-differences of the current pose and the point of its nearest neighboring trajectory
			double eps = 20; // valid check-radius when checking for the parrallelity to another part of the trajectory, [pixels], TODO: !!!!param!!!!
			int valid_room_index = 0; // used to find the interpolated paths, that are only computed for rooms with a valid path
			for(size_t room=0; room<paths.size(); ++room)
			{
				if(paths[room].size()==0)
					continue;

				std::vector<double> current_wall_angle_differences, current_trajectory_angle_differeneces;
				std::vector<int> current_revisit_times;
				for(std::vector<geometry_msgs::Pose2D>::iterator pose=paths[room].begin(); pose!=paths[room].end()-1; ++pose)
				{
					double dy = (pose+1)->y - pose->y;
					double dx = (pose+1)->x - pose->x;
					double norm = std::sqrt(dy*dy + dx*dx);
					if(norm==0)
						continue;
					dy = dy/norm;
					dx = dx/norm;
//					std::cout << "dx: " << dx << ", dy: " << dy << std::endl;

					// go in the directions of both normals and find the nearest wall
					int iteration_index = 0;
					bool hit_wall = false, hit_trajectory = false, exceeded_check_range = false;
					cv::Point2f n1(pose->x, pose->y), n2(pose->x, pose->y);
					cv::Point wall_pixel, trajectory_pixel;
					do
					{
						++iteration_index;

						// update normals
						n1.x -= dy;
						n1.y += dx;
						n2.x += dy;
						n2.y -= dx;

						// test if a wall/obstacle has been hit
						if(map.at<uchar>(n1)==0 && hit_wall==false)
						{
							hit_wall = true;
							wall_pixel = n1;
						}
						else if(map.at<uchar>(n2)==0 && hit_wall==false)
						{
							hit_wall = true;
							wall_pixel = n2;
						}

						// only check the parallelity to another trajectory, if the range hasn't been exceeded yet
						if(exceeded_check_range==false)
						{
							// test if another trajectory part has been hit, if the check-radius is still satisfied
							double dist1 = cv::norm(n1-cv::Point2f(pose->x, pose->y));
							double dist2 = cv::norm(n2-cv::Point2f(pose->x, pose->y));

							if(path_map.at<uchar>(n1)==127 && dist1<=eps && hit_trajectory==false)
							{
								hit_trajectory = true;
								trajectory_pixel = n1;
							}
							else if(path_map.at<uchar>(n2)==127 && dist2<=eps && hit_trajectory==false)
							{
								hit_trajectory = true;
								trajectory_pixel = n2;
							}

							// if both distances exceed the valid check range, mark as finished
							if(dist1>eps && dist2>eps)
								exceeded_check_range = true;
						}

//						cv::Mat test_map = map.clone();
//						cv::circle(test_map, cv::Point(pose->x, pose->y), 2, cv::Scalar(127), CV_FILLED);
//						cv::circle(test_map, cv::Point((pose+1)->x, (pose+1)->y), 2, cv::Scalar(127), CV_FILLED);
//						cv::circle(test_map, n1, 2, cv::Scalar(127), CV_FILLED);
//						cv::circle(test_map, n2, 2, cv::Scalar(127), CV_FILLED);
//						cv::imshow("normals", test_map);
//						cv::waitKey();
					}while((hit_wall==false || hit_trajectory==false) && iteration_index<=1000 && exceeded_check_range==false);

					// if a wall/obstacle was found, determine the gradient at this position and compare it to the direction of the path
//					double gradient;
					if(hit_wall==true)
					{
						double gradient = gradient_map.at<double>(wall_pixel);
						cv::Point2f grad_vector(std::cos(gradient), std::sin(gradient));
						double delta_theta = std::acos(grad_vector.x*dx + grad_vector.y*dy);
						current_wall_angle_differences.push_back(delta_theta);
					}

					// if another trajectory part could be found, determine the parallelity to it
					if(hit_trajectory==true)
					{
						// find the trajectory point in the interpolated path
						cv::Point2f world_neighbor((trajectory_pixel.x*datas.map_resolution_)+datas.map_origin_.position.x, (trajectory_pixel.y*datas.map_resolution_)+datas.map_origin_.position.y); // transform in world coordinates
						int pose_index = pose-paths[room].begin();
						int neighbor_index = -1;
						for(std::vector<geometry_msgs::Pose2D>::const_iterator neighbor=interpolated_paths[valid_room_index].begin(); neighbor!=interpolated_paths[valid_room_index].end(); ++neighbor)
						{
//							std::cout << *neighbor << ", " << world_neighbor << std::endl;
							if(world_neighbor.x==neighbor->x && world_neighbor.y==neighbor->y)
							{
//								std::cout << "gotz" << std::endl;
								neighbor_index = neighbor-interpolated_paths[valid_room_index].begin();
							}
						}
//						std::cout << "index: " << pose_index << ", n: " << neighbor_index << std::endl;

						// save the found index difference
						current_revisit_times.push_back(std::abs(pose_index-neighbor_index));

						// calculate the gradient-angle at the neighbor to get the difference
						// -->	check gradient to previous and next pose to get the minimal one
						double n_dx, n_dy;
						double delta_theta1 = 1e3, delta_theta2 = 1e3;
						if(neighbor_index<interpolated_paths[valid_room_index].size()-1) // neighbor not last node
						{
							n_dx = interpolated_paths[valid_room_index][neighbor_index+1].x-world_neighbor.x;
							n_dy = interpolated_paths[valid_room_index][neighbor_index+1].y-world_neighbor.y;
							norm = std::sqrt(n_dx*n_dx + n_dy*n_dy);
							n_dx = n_dx/norm;
							n_dy = n_dy/norm;
							delta_theta1 = std::acos(n_dx*dx + n_dy*dy);
						}
						if(neighbor_index>0) // neighbor not first node
						{
							n_dx = interpolated_paths[valid_room_index][neighbor_index-1].x-world_neighbor.x;
							n_dy = interpolated_paths[valid_room_index][neighbor_index-1].y-world_neighbor.y;
							norm = std::sqrt(n_dx*n_dx + n_dy*n_dy);
							n_dx = n_dx/norm;
							n_dy = n_dy/norm;
							delta_theta2 = std::acos(n_dx*dx + n_dy*dy);
						}
						if(delta_theta1<delta_theta2 && delta_theta1!=1e3)
						{
							current_trajectory_angle_differeneces.push_back(delta_theta1);
//							std::cout << delta_theta1 << std::endl;
						}
						else if(delta_theta2<=delta_theta1 && delta_theta2!=1e3)
						{
							current_trajectory_angle_differeneces.push_back(delta_theta2);
//							std::cout << delta_theta2 << std::endl;
						}
					}
				}
//				std::cout << "got all gradients" << std::endl;

				// save found values
				wall_angle_differences.push_back(current_wall_angle_differences);
				trajectory_angle_differeneces.push_back(current_trajectory_angle_differeneces);
				revisit_times.push_back(current_revisit_times);

				// increase path index
				++valid_room_index;
			}

			// calculate the mean and deviation of the angle differences and revisit times for each room and overall
			double average_wall_angle_difference = 0.0, average_trajectory_angle_difference = 0.0, average_revisit_times = 0.0;
			std::vector<double> room_wall_averages, room_trajectory_averages, room_revisit_averages;
			for(size_t room=0; room<wall_angle_differences.size(); ++room)
			{
				double current_room_average = std::accumulate(wall_angle_differences[room].begin(), wall_angle_differences[room].end(), 0.0);
				current_room_average /= wall_angle_differences.size();
				average_wall_angle_difference += current_room_average;
				room_wall_averages.push_back(current_room_average);
			}
			for(size_t room=0; room<trajectory_angle_differeneces.size(); ++room)
			{
				double current_room_average = std::accumulate(trajectory_angle_differeneces[room].begin(), trajectory_angle_differeneces[room].end(), 0.0);
				current_room_average /= trajectory_angle_differeneces.size();
				average_trajectory_angle_difference += current_room_average;
				room_trajectory_averages.push_back(current_room_average);
			}
			for(size_t room=0; room<revisit_times.size(); ++room)
			{
				double current_room_average = std::accumulate(revisit_times[room].begin(), revisit_times[room].end(), 0.0);
				current_room_average /= revisit_times.size();
				average_revisit_times += current_room_average;
				room_revisit_averages.push_back(current_room_average);
			}
			average_wall_angle_difference /= wall_angle_differences.size();
			average_trajectory_angle_difference /= trajectory_angle_differeneces.size();
			average_revisit_times /= revisit_times.size();
			double wall_deviation = 0.0, trajectory_deviation = 0.0, revisit_deviation = 0.0;
			for(size_t room=0; room<room_wall_averages.size(); ++room)
				wall_deviation += std::pow(room_wall_averages[room]-average_wall_angle_difference, 2.0);
			for(size_t room=0; room<room_trajectory_averages.size(); ++room)
				trajectory_deviation += std::pow(room_trajectory_averages[room]-average_trajectory_angle_difference, 2.0);
			for(size_t room=0; room<room_revisit_averages.size(); ++room)
				revisit_deviation += std::pow(room_revisit_averages[room]-average_revisit_times, 2.0);
			wall_deviation /= room_wall_averages.size();
			trajectory_deviation /= room_trajectory_averages.size();
			revisit_deviation /= room_revisit_averages.size();

			// 7. calculate the number of crossings related values
			double average_crossings = std::accumulate(numbers_of_crossings.begin(), numbers_of_crossings.end(), 0.0);
			average_crossings /= numbers_of_crossings.size();
			double deviation_crossings = 0.0;
			for(std::vector<int>::iterator cr=numbers_of_crossings.begin(); cr!=numbers_of_crossings.end(); ++cr)
				deviation_crossings += *cr;
			deviation_crossings /= numbers_of_crossings.size();

			// 8. calculate the subjective measure for the paths
			// TODO: !!!!!param!!!!!
			double subjective_measure = 1.0*average_pathlength + 1.0*average_computation_time + 1.0*average_wall_angle_difference
					+ 1.0*average_trajectory_angle_difference + 1.0*average_revisit_times + 1.0*average_crossings + 1.0*average_number_of_turns;
			subjective_measure /= 7.0;

			// TODO: number of rooms

			// TODO: print to file
			// print the found evaluation values to a local file
			std::stringstream output;
			output << "Expl" << config->exploration_algorithm_ << ", number of rooms: " << paths.size() << ", number of valid paths: "
					<< nonzero_paths << std::endl;
			output << "average calculation time [s]\t" << "calculation time deviation" << "overall pathlength [m]\t"
					<< "average pathlength [m]\t" << "average execution time [s]\t" << "execution time variance\t"
					<< "average number of turns\t" << "number of turns deviation\t" << "average covered area [m^2]\t"
					<< "covered area deviation\t" << "average coverage per pixel\t" << "coverage per pixel deviation\t"
					<< "average wall angle difference\t" << "wall angle difference deviation\t" << "average trajectory angle difference\t"
					<< "trajectory angle difference deviation\t" << "average time until traj. is near previous traj.\t" << "deviation of previous\t"
					<< "average number of crossings\t" << "deviation of crossings\t" << "subjective measure\t"<< std::endl;
			output << average_computation_time << "\t" << computation_time_devition << "\t" << overall_pathlength << "\t"
					<< average_pathlength << "\t" << average_execution_time << "\t" << execution_time_squared_variance << "\t"
					<< average_number_of_turns << "\t" << number_of_turns_deviation << "\t" << average_coverage_percentage << "\t"
					<< coverage_deviation << "\t" << average_coverage_number << "\t" << coverage_number_deviation << "\t"
					<< average_wall_angle_difference << "\t" << wall_deviation << "\t" << average_trajectory_angle_difference << "\t"
					<< trajectory_deviation << "\t" << average_revisit_times << "\t" << revisit_deviation << "\t"
					<< average_crossings << "\t" << deviation_crossings << "\t" << subjective_measure;

			std::string filename = data_storage_path + folder_path + datas.map_name_ + "_evaluations.txt";
			std::ofstream file(filename.c_str(), std::ofstream::out);
			if (file.is_open())
			{
				file << output.str();
			}
			else
				ROS_ERROR("Could not write to file '%s'.", filename.c_str());
			file.close();
		}
	}

	// function that plans one coverage path for the given room map
	bool planCoveragePath(const cv::Mat& room_map, const ExplorationData& evaluation_data, const ExplorationConfig& evaluation_configuration,
				ipa_building_msgs::RoomExplorationResultConstPtr& result_expl,
				const geometry_msgs::Polygon& min_max_points, const geometry_msgs::Polygon& region_of_interest)
	{
		sensor_msgs::Image map_msg;
		cv_bridge::CvImage cv_image;
		cv_image.encoding = "mono8";
		cv_image.image = room_map;
		cv_image.toImageMsg(map_msg);

		actionlib::SimpleActionClient<ipa_building_msgs::RoomExplorationAction> ac_exp("room_exploration_server", true);
		ROS_INFO("Waiting for action server to start.");
		ac_exp.waitForServer(); //will wait for infinite time
		ROS_INFO("Action server started.");

		ROS_INFO("Trying to connect to dynamic reconfigure server.");
		DynamicReconfigureClient drc_exp(node_handle_, "room_exploration_server/set_parameters", "room_exploration_server/parameter_updates");
		ROS_INFO("Done connecting to the dynamic reconfigure server.");
		if (evaluation_configuration.exploration_algorithm_==1)
		{
			drc_exp.setConfig("room_exploration_algorithm", 1);
			ROS_INFO("You have chosen the grid exploration method.");
		}
		else if(evaluation_configuration.exploration_algorithm_==2)
		{
			drc_exp.setConfig("room_exploration_algorithm", 2);
			ROS_INFO("You have chosen the boustrophedon exploration method.");
		}
		else if(evaluation_configuration.exploration_algorithm_==3)
		{
			drc_exp.setConfig("room_exploration_algorithm", 3);
			ROS_INFO("You have chosen the neural network exploration method.");
		}
		else if(evaluation_configuration.exploration_algorithm_==4)
		{
			drc_exp.setConfig("room_exploration_algorithm", 4);
			ROS_INFO("You have chosen the convexSPP exploration method.");
		}
		else if(evaluation_configuration.exploration_algorithm_==5)
		{
			drc_exp.setConfig("room_exploration_algorithm", 5);
			ROS_INFO("You have chosen the flow network exploration method.");
		}
		else if(evaluation_configuration.exploration_algorithm_==6)
		{
			drc_exp.setConfig("room_exploration_algorithm", 6);
			ROS_INFO("You have chosen the energy functional exploration method.");
		}
		else if(evaluation_configuration.exploration_algorithm_==7)
			{
				drc_exp.setConfig("room_exploration_algorithm", 7);
				ROS_INFO("You have chosen the voronoi exploration method.");
			}

		ipa_building_msgs::RoomExplorationGoal goal;
		goal.input_map = map_msg;
		geometry_msgs::Pose2D map_origin;
		map_origin.x = evaluation_data.map_origin_.position.x;
		map_origin.y = evaluation_data.map_origin_.position.y;
		goal.map_origin = map_origin;
		goal.starting_position = evaluation_data.robot_start_position_;
		goal.map_resolution = evaluation_data.map_resolution_;
		goal.robot_radius = evaluation_data.robot_radius_;
		goal.room_min_max = min_max_points;
		goal.field_of_view = evaluation_data.fow_points_;
		goal.region_of_interest_coordinates = region_of_interest;
		goal.return_path = true;
		goal.execute_path = false;
		ac_exp.sendGoal(goal);

		// wait for results for 1 hour
		bool finished;
		// higher timeout for the flowNetworkExplorator, because much slower than the others
		if(evaluation_configuration.exploration_algorithm_==5)
			finished = ac_exp.waitForResult(ros::Duration(3600));
		else
			finished = ac_exp.waitForResult(ros::Duration(1800));


		if (finished == false) //if it takes too long the server should be killed and restarted
		{
			std::cout << "action server took too long" << std::endl;
			std::string pid_cmd = "pidof room_exploration_server > room_exploration_evaluation/expl_srv_pid.txt";
			int pid_result = system(pid_cmd.c_str());
			std::ifstream pid_reader("room_exploration_evaluation/expl_srv_pid.txt");
			int value;
			std::string line;
			if (pid_reader.is_open())
			{
				while (getline(pid_reader, line))
				{
					std::istringstream iss(line);
					while (iss >> value)
					{
						std::cout << "PID of room_exploration_server: " << value << std::endl;
						std::stringstream ss;
						ss << "kill " << value;
						std::string kill_cmd = ss.str();
						int kill_result = system(kill_cmd.c_str());
						std::cout << "kill result: " << kill_result << std::endl;
					}
				}
				pid_reader.close();
				remove("room_exploration_evaluation/expl_srv_pid.txt");
			}
			else
			{
				std::cout << "missing logfile" << std::endl;
			}
			return false;
		}
		else
		{
			// retrieve solution
			result_expl = ac_exp.getResult();
			std::cout << "Finished coverage planning successfully!" << std::endl;

			// show success
			return true;
		}
	}
};

int main(int argc, char **argv)
{
	ros::init(argc, argv, "room_exploration_evaluation");
	ros::NodeHandle nh;

	const std::string test_map_path = ros::package::getPath("ipa_room_segmentation") + "/common/files/test_maps/";
	const std::string data_storage_path = "room_exploration_evaluation/";
	//ExplorationEvaluation(ros::NodeHandle& nh, const std::string& test_map_path, const std::string& data_storage_path,
//	const double robot_radius, const std::vector<int>& segmentation_algorithms, const std::vector<int>& exploration_algorithms,
//	const std::vector<geometry_msgs::Point32>& fow_points)
	std::vector<int> exploration_algorithms;
	for(int i=2; i<=2; ++i)
	{
		// choose which algorithms not to evaluate
		if(i==5)
			continue;

		exploration_algorithms.push_back(i);
	}
//	geometry_msgs::Point32 fov_point_1;// geometry_msgs::Point32(0.3, 0.3);
//	fov_point_1.x = 0.1;
//	fov_point_1.y = 0.5;
//	geometry_msgs::Point32 fov_point_2;// = geometry_msgs::Point32(0.3, -0.3);
//	fov_point_2.x = 0.1;
//	fov_point_2.y = -0.5;
//	geometry_msgs::Point32 fov_point_3;// = geometry_msgs::Point32(0.7, 0.7);
//	fov_point_3.x = 0.6;
//	fov_point_3.y = -0.75;
//	geometry_msgs::Point32 fov_point_4;// = geometry_msgs::Point32(0.7, -0.7);
//	fov_point_4.x = 0.6;
//	fov_point_4.y = 0.75;
	// coordinate system definition: x points in forward direction of robot and camera, y points to the left side  of the robot and z points upwards. x and y span the ground plane.
	// measures in [m]
	std::vector<geometry_msgs::Point32> fov_points(4);
	fov_points[0].x = 0.15;		// this field of view fits a Asus Xtion sensor mounted at 0.63m height (camera center) pointing downwards to the ground in a respective angle
	fov_points[0].y = 0.35;
	fov_points[1].x = 0.15;
	fov_points[1].y = -0.35;
	fov_points[2].x = 1.15;
	fov_points[2].y = -0.65;
	fov_points[3].x = 1.15;
	fov_points[3].y = 0.65;

	double robot_radius = 0.325;		// [m]
	ExplorationEvaluation ev(nh, test_map_path, data_storage_path, robot_radius, exploration_algorithms, fov_points);
	ros::shutdown();

	//exit
	return 0;
}
/*
 * Copyright (c) 2011, Willow Garage, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Willow Garage, Inc. nor the names of its
 *       contributors may be used to endorse or promote products derived from
 *       this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <ros/ros.h>
#include <pluginlib/class_list_macros.h>
#include <nodelet/nodelet.h>
#include <geometry_msgs/Twist.h>
#include <sensor_msgs/Image.h>
#include <visualization_msgs/Marker.h>
#include <turtlebot_msgs/SetFollowState.h>
#include <stdlib.h>
#include "dynamic_reconfigure/server.h"
#include "turtlebot_follower/FollowerConfig.h"
#include <cmvision/Blob.h>
#include <cmvision/Blobs.h>
#include <depth_image_proc/depth_traits.h>


namespace turtlebot_follower
{

    //* The turtlebot follower nodelet.
    /**
     * The turtlebot follower nodelet. Subscribes to point clouds
     * from the 3dsensor, processes them, and publishes command vel
     * messages.
     */
    class TurtlebotFollower : public nodelet::Nodelet
    {
        public:
            /*!
             * @brief The constructor for the follower.
             * Constructor for the follower.
             */

            /*
               2/12 BS - Modifying the goal z to test whether this affects the follow distance.
             */
            TurtlebotFollower() : min_y_(0.1), max_y_(0.5),
            min_x_(-0.2), max_x_(0.2),
            max_z_(0.8), goal_z_(1.2),
            z_scale_(1.0), x_scale_(5.0)
        {

        }

            ~TurtlebotFollower()
            {
                delete config_srv_;
            }

        private:
            double min_y_; /**< The minimum y position of the points in the box. */
            double max_y_; /**< The maximum y position of the points in the box. */
            double min_x_; /**< The minimum x position of the points in the box. */
            double max_x_; /**< The maximum x position of the points in the box. */
            double max_z_; /**< The maximum z position of the points in the box. */
            double goal_z_; /**< The distance away from the robot to hold the centroid */
            double z_scale_; /**< The scaling factor for translational robot speed */
            double x_scale_; /**< The scaling factor for rotational robot speed */
            bool   enabled_; /**< Enable/disable following; just prevents motor commands */
            int count;
            bool rotate_;
            int dir_;
            int TURN_THRES = 90;
            bool seesBlobs_;
            bool seesObstacle_;

            // Service for start/stop following
            ros::ServiceServer switch_srv_;

            // Dynamic reconfigure server
            dynamic_reconfigure::Server<turtlebot_follower::FollowerConfig>* config_srv_;

            /*!
             * @brief OnInit method from node handle.
             * OnInit method from node handle. Sets up the parameters
             * and topics.
             */
            virtual void onInit()
            {
                ros::NodeHandle& nh = getNodeHandle();
                ros::NodeHandle& private_nh = getPrivateNodeHandle();

                private_nh.getParam("min_y", min_y_);
                private_nh.getParam("max_y", max_y_);
                private_nh.getParam("min_x", min_x_);
                private_nh.getParam("max_x", max_x_);
                private_nh.getParam("max_z", max_z_);
                private_nh.getParam("goal_z", goal_z_);
                private_nh.getParam("z_scale", z_scale_);
                private_nh.getParam("x_scale", x_scale_);
                private_nh.getParam("enabled", enabled_);

                cmdpub_ = private_nh.advertise<geometry_msgs::Twist> ("cmd_vel", 1);
                markerpub_ = private_nh.advertise<visualization_msgs::Marker>("marker",1);
                bboxpub_ = private_nh.advertise<visualization_msgs::Marker>("bbox",1);
                pub_msg = private_nh.advertise<geometry_msgs::Twist> ("cmd_vel", 1);
                sub_= nh.subscribe<sensor_msgs::Image>("depth/image_rect", 1, &TurtlebotFollower::imagecb, this);
                //colorsub_= nh.subscribe<geometry_msgs::Twist>("follower_velocity",1, &TurtlebotFollower::colorcb, this);
                blobsSubscriber = nh.subscribe("/blobs", 1, &TurtlebotFollower::blobsCallBack,this);
                switch_srv_ = private_nh.advertiseService("change_state", &TurtlebotFollower::changeModeSrvCb, this);

                config_srv_ = new dynamic_reconfigure::Server<turtlebot_follower::FollowerConfig>(private_nh);
                dynamic_reconfigure::Server<turtlebot_follower::FollowerConfig>::CallbackType f =
                    boost::bind(&TurtlebotFollower::reconfigure, this, _1, _2);
                config_srv_->setCallback(f);
                rotate_=false;
                count = 0;
                dir_ = 1;
                seesObstacle_ = false;
            }

            void reconfigure(turtlebot_follower::FollowerConfig &config, uint32_t level)
            {
                min_y_ = config.min_y;
                max_y_ = config.max_y;
                min_x_ = config.min_x;
                max_x_ = config.max_x;
                max_z_ = config.max_z;
                goal_z_ = config.goal_z;
                z_scale_ = config.z_scale;
                x_scale_ = config.x_scale;
            }

            void blobsCallBack (const cmvision::Blobs& blobsIn)
            {
                /************************************************************
                 * These blobsIn.blobs[i].red, blobsIn.blobs[i].green, and blobsIn.blobs[i].blue values depend on the
                 * values those are provided in the colos.txt file.
                 * For example, the color file is like:
                 *
                 * [Colors]
                 * (255, 0, 0) 0.000000 10 RED
                 * (255, 255, 0) 0.000000 10 YELLOW
                 * [Thresholds]
                 * ( 127:187, 142:161, 175:197 )
                 * ( 47:99, 96:118, 162:175 )
                 *
                 * Now, if a red blob is found, then the blobsIn.blobs[i].red will be 255, and the others will be 0.
                 * Similarly, for yellow blob, blobsIn.blobs[i].red and blobsIn.blobs[i].green will be 255, and
                 blobsIn.blobs[i].blue will be 0.
                 ************************************************************/
                if(blobsIn.blob_count>0){
                    seesBlobs_ = true;
                }else{
                    seesBlobs_ = false;
                }

            }



            void colorcb(const geometry_msgs::TwistConstPtr & color_msg){
                geometry_msgs::TwistPtr cmd(new geometry_msgs::Twist());

                cmd->linear.x = color_msg->linear.x;
                ROS_INFO_THROTTLE(1, "angular.z: %f", cmd->angular.z);
                cmdpub_.publish(cmd);

            }
            /*!
             * @brief Callback for point clouds.
             * Callback for depth images. It finds the centroid
             * of the points in a box in the center of the image. 
             * Publishes cmd_vel messages with the goal from the image.
             * @param cloud The point cloud message.
             */
            void imagecb(const sensor_msgs::ImageConstPtr& depth_msg)
            {

                // Precompute the sin function for each row and column
                uint32_t image_width = 0.5*(depth_msg->width);
                float x_radians_per_pixel = 60.0/57.0/image_width;
                float sin_pixel_x[image_width];
                for (int x = 0; x < image_width; ++x) {
                    sin_pixel_x[x] = sin((x - image_width/ 2.0)  * x_radians_per_pixel);
                }

                uint32_t image_height = (depth_msg->height);
                float y_radians_per_pixel = 45.0/57.0/image_width;
                float sin_pixel_y[image_height];
                for (int y = 0; y < image_height; ++y) {
                    // Sign opposite x for y up values
                    sin_pixel_y[y] = sin((image_height/ 2.0 - y)  * y_radians_per_pixel);
                }

                //X,Y,Z of the centroid
                float x = 0.0;
                float y = 0.0;
                float z = 1e6;
                //Number of points observed
                unsigned int n = 0;

                //Iterate through all the points in the region and find the average of the position
                const float* depth_row = reinterpret_cast<const float*>(&depth_msg->data[0]);
                int row_step = depth_msg->step / sizeof(float);
                for (int v = 0; v < (int)depth_msg->height; ++v, depth_row += row_step)
                {
                    for (int u = 0; u < (int)depth_msg->width; ++u)
                    {
                        float depth = depth_image_proc::DepthTraits<float>::toMeters(depth_row[u]);
                        if (!depth_image_proc::DepthTraits<float>::valid(depth) || depth > max_z_) continue;
                        float y_val = sin_pixel_y[v] * depth;
                        float x_val = sin_pixel_x[u] * depth;
                        if ( y_val > min_y_ && y_val < max_y_ &&
                                x_val > min_x_ && x_val < max_x_)
                        {
                            x += x_val;
                            y += y_val;
                            z = std::min(z, depth); //approximate depth as forward.
                            n++;
                        }
                    }
                }

                //If there are points, find the centroid and calculate the command goal.
                //If there are no points, simply publish a stop goal.
                if (n>3000 || seesObstacle_) //obstacle detected
                {
                    x /= n;
                    y /= n;
//                    ROS_INFO("goal_z_: %f", goal_z_);

//                    ROS_INFO("Centroid at %f %f %f with %d points", x, y, z, n);
                    publishMarker(x, y, z);

                    // obstacle detected.
                    if (enabled_)
                    {
                        // obstacle is not goal, go around
                        if(!seesBlobs_){
                            seesObstacle_ = true;
                            if(count < TURN_THRES ){ // turn left
                                geometry_msgs::TwistPtr cmd(new geometry_msgs::Twist());
                                cmd->angular.z = 0.6;
                                ROS_INFO_THROTTLE(1, "angular.z: %f", cmd->angular.z);
                                cmdpub_.publish(cmd);
                                count++;
                                ROS_INFO("count: %d",count);
                            }else if(count < 2 * TURN_THRES){ // move forward
                                ROS_INFO("count > 2*TURN_THRES");
                                geometry_msgs::TwistPtr cmd(new geometry_msgs::Twist());
                                cmd->linear.x = 0.3;
                                cmdpub_.publish(cmd);
                                count++;
                            }
                            else{//at this time, obstacle should be out of its sight
                                ROS_INFO("reset count");
                                seesObstacle_ = false;
                                count = 0;
                                geometry_msgs::TwistPtr cmd(new geometry_msgs::Twist());
                                cmdpub_.publish(cmd);
                            }
                        }else{ // obstacle = goal
                            ROS_INFO("reached goal");
                            geometry_msgs::TwistPtr cmd(new geometry_msgs::Twist());
                            cmdpub_.publish(cmd);
                        }
                    }
                }
                else // no obstacle
                {
                    if(seesBlobs_){ // goal found, move toward goal
                        count = 0;
                        ROS_INFO("blob found");
                        geometry_msgs::TwistPtr cmd(new geometry_msgs::Twist());
                        cmd->linear.x = 0.5;
                        cmdpub_.publish(cmd);

                    }else{ // initial state, rotate to find goal
                        ROS_INFO("blob not found");         
                        geometry_msgs::TwistPtr cmd(new geometry_msgs::Twist());
                        cmd->angular.z = 0.5;
                        cmdpub_.publish(cmd);
                    }
                }
                publishBbox();

            }

            bool changeModeSrvCb(turtlebot_msgs::SetFollowState::Request& request,
                    turtlebot_msgs::SetFollowState::Response& response)
            {
                if ((enabled_ == true) && (request.state == request.STOPPED))
                {
                    ROS_INFO("Change mode service request: following stopped");
                    cmdpub_.publish(geometry_msgs::TwistPtr(new geometry_msgs::Twist()));
                    enabled_ = false;
                }
                else if ((enabled_ == false) && (request.state == request.FOLLOW))
                {
                    ROS_INFO("Change mode service request: following (re)started");
                    enabled_ = true;
                }

                response.result = response.OK;
                return true;
            }

            void publishMarker(double x,double y,double z)
            {
                visualization_msgs::Marker marker;
                marker.header.frame_id = "/camera_rgb_optical_frame";
                marker.header.stamp = ros::Time();
                marker.ns = "my_namespace";
                marker.id = 0;
                marker.type = visualization_msgs::Marker::SPHERE;
                marker.action = visualization_msgs::Marker::ADD;
                marker.pose.position.x = x;
                marker.pose.position.y = y;
                marker.pose.position.z = z;
                marker.pose.orientation.x = 0.0;
                marker.pose.orientation.y = 0.0;
                marker.pose.orientation.z = 0.0;
                marker.pose.orientation.w = 1.0;
                marker.scale.x = 0.2;
                marker.scale.y = 0.2;
                marker.scale.z = 0.2;
                marker.color.a = 1.0;
                marker.color.r = 1.0;
                marker.color.g = 0.0;
                marker.color.b = 0.0;
                //only if using a MESH_RESOURCE marker type:
                markerpub_.publish( marker );
            }

            void publishBbox()
            {
                double x = (min_x_ + max_x_)/2;
                double y = (min_y_ + max_y_)/2;
                double z = (0 + max_z_)/2;

                double scale_x = (max_x_ - x)*2;
                double scale_y = (max_y_ - y)*2;
                double scale_z = (max_z_ - z)*2;

                visualization_msgs::Marker marker;
                marker.header.frame_id = "/camera_rgb_optical_frame";
                marker.header.stamp = ros::Time();
                marker.ns = "my_namespace";
                marker.id = 1;
                marker.type = visualization_msgs::Marker::CUBE;
                marker.action = visualization_msgs::Marker::ADD;
                marker.pose.position.x = x;
                marker.pose.position.y = -y;
                marker.pose.position.z = z;
                marker.pose.orientation.x = 0.0;
                marker.pose.orientation.y = 0.0;
                marker.pose.orientation.z = 0.0;
                marker.pose.orientation.w = 1.0;
                marker.scale.x = scale_x;
                marker.scale.y = scale_y;
                marker.scale.z = scale_z;
                marker.color.a = 0.5;
                marker.color.r = 0.0;
                marker.color.g = 1.0;
                marker.color.b = 0.0;
                //only if using a MESH_RESOURCE marker type:
                bboxpub_.publish( marker );
            }
            ros::Subscriber blobsSubscriber;
            ros::Subscriber sub_;
            ros::Subscriber colorsub_;
            ros::Publisher pub_msg;
            ros::Publisher cmdpub_;
            ros::Publisher markerpub_;
            ros::Publisher bboxpub_;
    };

    PLUGINLIB_DECLARE_CLASS(turtlebot_follower, TurtlebotFollower, turtlebot_follower::TurtlebotFollower, nodelet::Nodelet);

}

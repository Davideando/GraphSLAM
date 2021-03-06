#include "utils.hpp"
#include "scanner.hpp"
#include <iostream>

// #### TUNING CONSTANTS START
// Thresholds for voting for keyframe:
int gicp_maximum_iterations;
double gicp_maximum_correspondence_distance, gicp_transformation_epsilon, gicp_euclidean_fitness_epsilon;

int loop_closure_skip;
double fitness_keyframe_threshold, fitness_loop_threshold, distance_threshold, rotation_threshold;

// Uncertainty model constants
double k_disp_disp, k_rot_disp, k_rot_rot;
double sigma_xy, sigma_th;

// #### TUNING CONSTANTS END

ros::Publisher registration_pub;
ros::Publisher pointcloud_debug_pub;
ros::Publisher delta_pub;
ros::ServiceClient keyframe_last_client;
ros::ServiceClient keyframe_closest_client;

// GICP algorithm
//pcl::GeneralizedIterativeClosestPoint<pcl::PointXYZ, pcl::PointXYZ> gicp;
pcl::IterativeClosestPoint<pcl::PointXYZ, pcl::PointXYZ> gicp;

Eigen::Matrix4f carry_transform; // The transform of the last align which is passed to the next align as initial guess
unsigned int loop_closure_skip_count;

// Helper functions

using namespace scanner;

/**
 * \brief Align two pointclouds, with transform prior.
 *
 * Format the results in a compact structure `Alignement`
 */
Alignement gicp_register(const sensor_msgs::PointCloud2 input_1, const sensor_msgs::PointCloud2 input_2, Eigen::Matrix4f& transform){

    // assign inputs
    pcl::PointCloud<pcl::PointXYZ>::Ptr pointcloud_1 = format_pointcloud(input_1);
    pcl::PointCloud<pcl::PointXYZ>::Ptr pointcloud_2 = format_pointcloud(input_2);
    gicp.setInputSource(pointcloud_1);
    gicp.setInputTarget(pointcloud_2);

    // align
    pcl::PointCloud<pcl::PointXYZ>::Ptr pointcloud_transform(new pcl::PointCloud<pcl::PointXYZ>);
    gicp.align(*pointcloud_transform, transform);

    Alignement output;
    output.convergence_state = gicp.getConvergeCriteria()->getConvergenceState();
    output.converged = gicp.hasConverged();
    output.fitness = gicp.getFitnessScore();

//    ROS_INFO("Alignement converged: (%d) with fitness: %f", output.converged, output.fitness);

    if (gicp.hasConverged())
    {
        transform = gicp.getFinalTransformation();

        // Get transformation Delta and compute its covariance
        output.transform = transform;
        geometry_msgs::Pose2D transform_Delta = make_Delta(transform);
        Eigen::MatrixXd covariance_Delta = compute_covariance(sigma_xy, sigma_th);
        output.Delta = create_Pose2DWithCovariance_msg(transform_Delta, covariance_Delta);
    }

    return output;
}

/**
 * \brief Align two pointclouds, without transform prior.
 *
 * Format the results in a compact structure `Alignement`
 */
Alignement gicp_register(const sensor_msgs::PointCloud2 input_1, const sensor_msgs::PointCloud2 input_2){
    Eigen::Matrix4f guess_null(Eigen::Matrix4f::Identity());
    return gicp_register(input_1, input_2, guess_null);
}



/**
 * \brief Policy for creating keyframes
 */
bool vote_for_keyframe(const common::Pose2DWithCovariance Delta, const double fitness)
{
    if (fitness > fitness_keyframe_threshold) // fitness
        return true;
    if (fabs(Delta.pose.theta) > rotation_threshold) // rotation
        return true;
    if ((Delta.pose.x*Delta.pose.x+Delta.pose.y*Delta.pose.y) > distance_threshold*distance_threshold) // translation
        return true;

    return false;
}


// Node functions

/**
 * \brief Callback at the reception of a laser scan
 *
 * This function performs all the logic of this node. It decides whether:
 *   - The first keyframe is to be created,
 *   - A new keyframe is to be created
 *   - A loop closure is to be searched and created
 *
 * It publishes all the results in a unique `Registration` message.
 */
void scanner_callback(const sensor_msgs::LaserScan& input)
{

    // message to publish -- empty
    common::Registration output;

    // clear flags:
    output.first_frame_flag     = false;
    output.keyframe_flag        = false;
    output.loop_closure_flag    = false;

    // request last KF
    common::LastKeyframe keyframe_last_request;
    bool keyframe_last_request_returned = keyframe_last_client.call(keyframe_last_request);

    // Case of first frame
    if (!keyframe_last_request_returned)
    {
        ROS_INFO("### NO LAST KEYFRAME FOUND : ASSUME FIRST KEYFRAME ###");

        // Set flags, assign pointcloud
        output.first_frame_flag         = true;
        output.keyframe_new.scan        = input;
        output.keyframe_new.pointcloud  = scan_to_pointcloud(input);
    }

    // Case of other frames
    if (keyframe_last_request_returned)
    {
        // gather pointclouds
        sensor_msgs::PointCloud2 input_pointcloud = scan_to_pointcloud(input);
        sensor_msgs::PointCloud2 keyframe_last_pointcloud = keyframe_last_request.response.keyframe_last.pointcloud;

        // Do align
        double start = ros::Time::now().toSec();
        gicp.setMaxCorrespondenceDistance(0.5); // fine for close range
        Alignement alignement_last = gicp_register(input_pointcloud, keyframe_last_pointcloud, carry_transform);
        double end = ros::Time::now().toSec();

        // compose output message for KF creation
        output.keyframe_flag            = vote_for_keyframe(alignement_last.Delta, alignement_last.fitness);
        output.keyframe_new.ts          = input.header.stamp;
        output.keyframe_new.pointcloud  = input_pointcloud;
        output.keyframe_new.scan        = input;
        output.keyframe_last            = keyframe_last_request.response.keyframe_last;
        output.factor_new.id_1          = keyframe_last_request.response.keyframe_last.id;
        output.factor_new.id_2          = output.keyframe_new.id;
        output.factor_new.delta         = alignement_last.Delta;

        // Keyframe creation
        if (output.keyframe_flag)
        {
        	ROS_INFO("RG: align time: %f; fitness: %f", end - start, alignement_last.fitness);
            ROS_INFO_STREAM("RG: convergence state: " << convergence_text(alignement_last.convergence_state)); //convergence_text(alignement_loop.convergence_state));
            ROS_INFO("RG: Delta: %f %f %f", alignement_last.Delta.pose.x, alignement_last.Delta.pose.y, alignement_last.Delta.pose.theta);
            carry_transform.setIdentity();

            // Check for loop closures only if on Keyframes
            loop_closure_skip_count++;
            if (loop_closure_skip_count >= loop_closure_skip) // only try once in a while
            {

                // request closest KF to test for loop closure
                common::ClosestKeyframe keyframe_closest_request;
                keyframe_closest_request.request.keyframe_last = keyframe_last_request.response.keyframe_last;
                bool keyframe_closest_request_returned = keyframe_closest_client.call(keyframe_closest_request);

                if (keyframe_closest_request_returned)
                {
                    // compute prior transform between the 2 keyframes
                    Eigen::Matrix4f T_last = make_transform(keyframe_last_request.response.keyframe_last.pose_opti.pose);
                    Eigen::Matrix4f T_loop = make_transform(keyframe_closest_request.response.keyframe_closest.pose_opti.pose);
                    Eigen::Matrix4f loop_transform = T_last.inverse()*T_loop;

                    // get pointcloud
                    sensor_msgs::PointCloud2 keyframe_closest_pointcloud =
                            keyframe_closest_request.response.keyframe_closest.pointcloud;

                    // Do align
                    start = ros::Time::now().toSec();
                    gicp.setMaxCorrespondenceDistance(1.0); // coarse for loop closure
                    Alignement alignement_loop = gicp_register(keyframe_closest_pointcloud, keyframe_last_pointcloud, loop_transform);
                    end = ros::Time::now().toSec();

                    // print some stuff
                    ROS_INFO("LC: align time: %f; fitness: %f", end - start, alignement_loop.fitness);
                    ROS_INFO_STREAM("LC: convergence state: " << convergence_text(alignement_loop.convergence_state));
                    ROS_INFO("LC: Delta: %f %f %f", alignement_loop.Delta.pose.x, alignement_loop.Delta.pose.y, alignement_loop.Delta.pose.theta);

                    // compose output message
                    output.loop_closure_flag    = (alignement_loop.converged && alignement_loop.fitness < fitness_loop_threshold);
                    output.keyframe_last        = keyframe_last_request.response.keyframe_last;
                    output.keyframe_loop        = keyframe_closest_request.response.keyframe_closest;
                    output.factor_loop.id_1     = keyframe_last_request.response.keyframe_last.id;
                    output.factor_loop.id_2     = keyframe_closest_request.response.keyframe_closest.id;
                    output.factor_loop.delta    = alignement_loop.Delta;

                    if (output.loop_closure_flag)
                        loop_closure_skip_count = 0;

                } // keyframe closest returned
            } // loop closure skip count
        } // keyframe_flag
    } // other keyframes

    // publish registration message
    registration_pub.publish(output);

}

/**
 * \brief Main process
 *
 * Initialize all services, subscribers and publishers.
 *
 * Initialize and setup the ICP alignment algorithm
 */
int main(int argc, char** argv) {
  ros::init(argc, argv, "scanner");
  ros::NodeHandle n;

  ros::Subscriber scanner_sub = n.subscribe("/base_scan", 1, scanner_callback);

  delta_pub = n.advertise<geometry_msgs::Pose2D>("/scanner/delta", 1);
  
  registration_pub  = n.advertise<common::Registration>("/scanner/registration", 1);
  pointcloud_debug_pub = n.advertise<sensor_msgs::PointCloud2>("/scanner/debug_pointcloud", 1);

  keyframe_last_client = n.serviceClient<common::LastKeyframe>("/graph/last_keyframe");
  keyframe_closest_client = n.serviceClient<common::ClosestKeyframe>("/graph/closest_keyframe");

  // Setup ICP algorithm
  gicp.setUseReciprocalCorrespondences(true);
  //  gicp.setCorrespondenceRandomness();
  //  gicp.setRotationEpsilon();

  // ### rosparam get gicp_maximum_iterations ###
  if(ros::param::get("/scanner/gicp_maximum_iterations", gicp_maximum_iterations)) {
    gicp.setMaximumIterations(gicp_maximum_iterations);
    ROS_INFO("ROSPARAM: [LOADED] /scanner/gicp_maximum_iterations = %d", gicp_maximum_iterations);
  } else {
    gicp_maximum_iterations = 50; // ICP example 50
    gicp.setMaximumIterations(gicp_maximum_iterations);
    ROS_WARN("ROSPARAM: [NOT LOADED][DEFAULT SET] /scanner/gicp_maximum_iterations = %d", gicp_maximum_iterations);
  }

  // ### rosparam get gicp_maximum_correspondence_distance ###
  if(ros::param::get("/scanner/gicp_maximum_correspondence_distance", gicp_maximum_correspondence_distance)) {
    gicp.setMaxCorrespondenceDistance(gicp_maximum_correspondence_distance);
    ROS_INFO("ROSPARAM: [LOADED] /scanner/gicp_maximum_correspondence_distance = %f",
	     gicp_maximum_correspondence_distance);
  } else {
    gicp_maximum_correspondence_distance = 0.05; // ICP example 0.05
    gicp.setMaxCorrespondenceDistance(gicp_maximum_correspondence_distance);
    ROS_WARN("ROSPARAM: [NOT LOADED][DEFAULT SET] /scanner/gicp_maximum_correspondence_distance = %f",
	     gicp_maximum_correspondence_distance);
  }

  gicp_transformation_epsilon = 1e-8; // ICP example 1e-8
  gicp.setTransformationEpsilon(gicp_transformation_epsilon); 

  // ### rosparam get gicp_euclidean_fitness_epsilon ###
  if(ros::param::get("/scanner/gicp_euclidean_fitness_epsilon", gicp_euclidean_fitness_epsilon)) {
    gicp.setEuclideanFitnessEpsilon(gicp_euclidean_fitness_epsilon);
    ROS_INFO("ROSPARAM: [LOADED] /scanner/gicp_euclidean_fitness_epsilon = %f", gicp_euclidean_fitness_epsilon);
  } else {
    gicp_euclidean_fitness_epsilon = 1.0; // ICP example 1
    gicp.setEuclideanFitnessEpsilon(gicp_euclidean_fitness_epsilon); 
    ROS_WARN("ROSPARAM: [NOT LOADED][DEFAULT SET] /scanner/gicp_euclidean_fitness_epsilon = %f",
	     gicp_euclidean_fitness_epsilon);
  }

  // ### rosparam get fitness_keyframe_threshold ###
  if(ros::param::get("/scanner/fitness_keyframe_threshold", fitness_keyframe_threshold)) {
    ROS_INFO("ROSPARAM: [LOADED] /scanner/fitness_keyframe_threshold = %f", fitness_keyframe_threshold);
  } else {
    fitness_keyframe_threshold = 1.5; 
    ROS_WARN("ROSPARAM: [NOT LOADED][DEFAULT SET] /scanner/fitness_keyframe_threshold = %f",
	     fitness_keyframe_threshold);
  }

  // ### rosparam get fitness_loop_threshold ###
  if(ros::param::get("/scanner/fitness_loop_threshold", fitness_loop_threshold)) {
    ROS_INFO("ROSPARAM: [LOADED] /scanner/fitness_loop_threshold = %f", fitness_loop_threshold);
  } else {
    fitness_loop_threshold = 4.5; 
    ROS_WARN("ROSPARAM: [NOT LOADED][DEFAULT SET] /scanner/fitness_loop_threshold = %f", fitness_loop_threshold);
  }

  // ### rosparam get distance_threshold ###
  if(ros::param::get("/scanner/distance_threshold", distance_threshold)) {
    ROS_INFO("ROSPARAM: [LOADED] /scanner/distance_threshold = %f", distance_threshold);
  } else {
    distance_threshold = 1.0; 
    ROS_WARN("ROSPARAM: [NOT LOADED][DEFAULT SET] /scanner/distance_threshold = %f", distance_threshold);
  }

  // ### rosparam get rotation_threshold ###
  if(ros::param::get("/scanner/rotation_threshold", rotation_threshold)) {
    ROS_INFO("ROSPARAM: [LOADED] /scanner/rotation_threshold = %f", rotation_threshold);
  } else {
    rotation_threshold = 1.0; 
    ROS_WARN("ROSPARAM: [NOT LOADED][DEFAULT SET] /scanner/rotation_threshold = %f", rotation_threshold);
  }

  // ### rosparam get loop_closure_skip ###
  if(ros::param::get("/scanner/loop_closure_skip", loop_closure_skip)) {
    ROS_INFO("ROSPARAM: [LOADED] /scanner/loop_closure_skip = %d", loop_closure_skip);
  } else {
    loop_closure_skip = 4; 
    ROS_WARN("ROSPARAM: [NOT LOADED][DEFAULT SET] /scanner/loop_closure_skip = %d", loop_closure_skip);
  }

  // ### rosparam get k_disp_disp ###
  if(ros::param::get("/scanner/k_disp_disp", k_disp_disp)) {
    ROS_INFO("ROSPARAM: [LOADED] /scanner/k_disp_disp = %f", k_disp_disp);
  } else {
    k_disp_disp = 0.001; 
    ROS_WARN("ROSPARAM: [NOT LOADED][DEFAULT SET] /scanner/k_disp_disp = %f", k_disp_disp);
  }

  // ### rosparam get k_rot_disp ###
  if(ros::param::get("/scanner/k_rot_disp", k_rot_disp)) {
    ROS_INFO("ROSPARAM: [LOADED] /scanner/k_rot_disp = %f", k_rot_disp);
  } else {
    k_rot_disp = 0.001; 
    ROS_WARN("ROSPARAM: [NOT LOADED][DEFAULT SET] /scanner/k_rot_disp = %f", k_rot_disp);
  }

  // ### rosparam get k_rot_rot ###
  if(ros::param::get("/scanner/k_rot_rot", k_rot_rot)) {
    ROS_INFO("ROSPARAM: [LOADED] /scanner/k_rot_rot = %f", k_rot_rot);
  } else {
    k_rot_rot = 0.001; 
    ROS_WARN("ROSPARAM: [NOT LOADED][DEFAULT SET] /scanner/k_rot_rot = %f", k_rot_rot);
  }

  // ### rosparam get sigma_xy ###
  if(ros::param::get("/scanner/sigma_xy", sigma_xy)) {
    ROS_INFO("ROSPARAM: [LOADED] /scanner/sigma_xy = %f", sigma_xy);
  } else {
    sigma_xy = 0.002; 
    ROS_WARN("ROSPARAM: [NOT LOADED][DEFAULT SET] /scanner/sigma_xy = %f", sigma_xy);
  }

  // ### rosparam get sigma_th ###
  if(ros::param::get("/scanner/sigma_th", sigma_th)) {
    ROS_INFO("ROSPARAM: [LOADED] /scanner/sigma_th = %f", sigma_th);
  } else {
    sigma_th = 0.001; 
    ROS_WARN("ROSPARAM: [NOT LOADED][DEFAULT SET] /scanner/sigma_th = %f", sigma_th);
  }

  // Spy ICP convergence criteria:
  ROS_INFO("ICP: max iter sim transf: %d", gicp.getConvergeCriteria()->getMaximumIterationsSimilarTransforms());
  ROS_INFO("ICP: fail after max iter: %d ", gicp.getConvergeCriteria()->getFailureAfterMaximumIterations());
  ROS_INFO("ICP: abs MSE : %f [x1e8]", 1e8*gicp.getConvergeCriteria()->getAbsoluteMSE());
  ROS_INFO("ICP: rel MSE : %f ", gicp.getConvergeCriteria()->getRelativeMSE());
  ROS_INFO("ICP: rot th  : %f [rad]", acos(gicp.getConvergeCriteria()->getRotationThreshold()));
  ROS_INFO("ICP: trans th: %f [m]", sqrt(gicp.getConvergeCriteria()->getTranslationThreshold()));
  ROS_INFO("ICP: max iter: %d ", gicp.getConvergeCriteria()->getMaximumIterations());
  ROS_INFO("ICP: RANSAC iter: %d ", int(gicp.getRANSACIterations()));

  gicp.getConvergeCriteria()->setMaximumIterationsSimilarTransforms(10);
  ROS_INFO("ICP: max iter sim transf: %d", gicp.getConvergeCriteria()->getMaximumIterationsSimilarTransforms());

  //  gicp.setRANSACIterations(10);
  //  ROS_INFO("ICP: RANSAC iter: %d ", int(gicp.getRANSACIterations()));

  //  gicp.getConvergeCriteria()->setFailureAfterMaximumIterations(true);
  //  ROS_INFO("ICP: fail after max iter: %d ", gicp.getConvergeCriteria()->getFailureAfterMaximumIterations());

  carry_transform.setIdentity();
  loop_closure_skip_count = 0;

  ros::spin();
  return 0;
}

#include <iostream>

#include <boost/filesystem.hpp>
#include <Eigen/Core>

#include <pcl/point_types.h>
#include <pcl/io/ply_io.h>
#include <pcl/kdtree/kdtree_flann.h>
#include <pcl/features/normal_3d.h>
#include <pcl/surface/gp3.h>

#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/viz/vizcore.hpp>
#include <kfusion/kinfu.hpp>
#include <io/capture.hpp>

using namespace kfusion;


struct KinFuApp
{
	static void KeyboardCallback(const cv::viz::KeyboardEvent& event, void* pthis)
	{
		KinFuApp& kinfu = *static_cast<KinFuApp*>(pthis);

		if(event.action != cv::viz::KeyboardEvent::KEY_DOWN)
			return;

		if(event.code == 't' || event.code == 'T')
			kinfu.take_cloud(*kinfu.kinfu_);

		if(event.code == 'i' || event.code == 'I')
			kinfu.iteractive_mode_ = !kinfu.iteractive_mode_;
	}

	KinFuApp(OpenNISource& source) : 
		exit_ (false),  iteractive_mode_(false), capture_ (source)
	{
		KinFuParams params = KinFuParams::default_params();
		kinfu_ = KinFu::Ptr( new KinFu(params) );

		capture_.setRegistration(true);

		cv::viz::WCube cube(cv::Vec3d::all(0), cv::Vec3d(params.volume_size), true, cv::viz::Color::apricot());
		viz.showWidget("cube", cube, params.volume_pose);
		viz.showWidget("coor", cv::viz::WCoordinateSystem(0.1));
		viz.registerKeyboardCallback(KeyboardCallback, this);
	}

	void show_depth(const cv::Mat& depth)
	{
		cv::Mat display;
		//cv::normalize(depth, display, 0, 255, cv::NORM_MINMAX, CV_8U);
		depth.convertTo(display, CV_8U, 255.0/4000);
		cv::imshow("Depth", display);
	}

	void show_raycasted(KinFu& kinfu)
	{
		const int mode = 3;
		if (iteractive_mode_)
			kinfu.renderImage(view_device_, viz.getViewerPose(), mode);
		else
			kinfu.renderImage(view_device_, mode);

		view_host_.create(view_device_.rows(), view_device_.cols(), CV_8UC4);
		view_device_.download(view_host_.ptr<void>(), view_host_.step);
		cv::imshow("Scene", view_host_);
	}

	void take_cloud(KinFu& kinfu)
	{
		cuda::DeviceArray<Point> cloud = kinfu.tsdf().fetchCloud(cloud_buffer);
		cv::Mat cloud_host(1, (int)cloud.size(), CV_32FC4);
		cloud.download(cloud_host.ptr<Point>());
		viz.showWidget("cloud", cv::viz::WCloud(cloud_host));
		//viz.showWidget("cloud", cv::viz::WPaintedCloud(cloud_host));
	}

	void generate_mesh(KinFu& kinfu)
	{
		std::cout << "\nGetting mesh... " << std::endl; 

		//////////////////////////////////////////////////////////////////////////
		// Download point clouds from device
		Vec3i dims = kinfu.tsdf().getDims();
		if (!cloud_buffer.empty())
			cloud_buffer.release();
		cloud_buffer.create(dims[0]*dims[1]*dims[2]);
		cuda::DeviceArray<Point> cloud_device = kinfu.tsdf().fetchCloud(cloud_buffer);

		pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_pcl (new pcl::PointCloud<pcl::PointXYZ>);
		
		cv::Mat cloud_host(1, (int)cloud_device.size(), CV_32FC4);
		cloud_device.download(cloud_host.ptr<Point>());
		//cloud_device.download(cloud->begin());

		//#pragma omp parallel for
		for (size_t i=0;i<cloud_device.size();++i) {
			pcl::PointXYZ pnt;
			cv::Vec4f pnt_host = cloud_host.at<cv::Vec4f>(0,i);
			pnt.x = pnt_host[0];
			pnt.y = pnt_host[1];
			pnt.z = pnt_host[2];
			cloud_pcl->push_back(pnt);
		}
		
		//////////////////////////////////////////////////////////////////////////
		// Normal estimation*
		pcl::NormalEstimation<pcl::PointXYZ, pcl::Normal> n;
		pcl::PointCloud<pcl::Normal>::Ptr normals (new pcl::PointCloud<pcl::Normal>);
		pcl::search::KdTree<pcl::PointXYZ>::Ptr tree (new pcl::search::KdTree<pcl::PointXYZ>);
		tree->setInputCloud (cloud_pcl);
		n.setInputCloud (cloud_pcl);
		n.setSearchMethod (tree);
		n.setKSearch (50);
		n.compute (*normals);
		//* normals should not contain the point normals + surface curvatures

		// Concatenate the XYZ and normal fields*
		pcl::PointCloud<pcl::PointNormal>::Ptr cloud_with_normals (new pcl::PointCloud<pcl::PointNormal>);
		pcl::concatenateFields (*cloud_pcl, *normals, *cloud_with_normals);
		//* cloud_with_normals = cloud + normals
				
		////////////////////////////////////////////////////////////////////////
		// Create search tree*
		pcl::search::KdTree<pcl::PointNormal>::Ptr tree2 (new pcl::search::KdTree<pcl::PointNormal>);
		tree2->setInputCloud (cloud_with_normals);

		// Initialize objects
		pcl::GreedyProjectionTriangulation<pcl::PointNormal> gp3;

		// Set the maximum distance between connected points (maximum edge length)
		gp3.setSearchRadius (0.1);

		// Set typical values for the parameters
		gp3.setMu (2.5);
		gp3.setMaximumNearestNeighbors (500);
		gp3.setMaximumSurfaceAngle(M_PI/4); // 45 degrees
		gp3.setMinimumAngle(M_PI/18); // 10 degrees
		gp3.setMaximumAngle(2*M_PI/3); // 120 degrees
		gp3.setNormalConsistency(false);

		// Get result
		gp3.setInputCloud (cloud_with_normals);
		gp3.setSearchMethod (tree2);
		gp3.reconstruct (mesh_);

	}

	void write_mesh(void)
	{
		if (!mesh_.polygons.empty())
		{
			std::cout << "Saving mesh to 'mesh.ply'... " << std::endl;
			pcl::io::savePLYFile("./mesh.ply", mesh_);
		}	
		else
		{
			std::cerr << "Mesh is empty !" << std::endl;
		}
	}

	bool execute()
	{
		KinFu& kinfu = *kinfu_;
		cv::Mat depth, image;
		double time_ms = 0;
		bool has_image = false;

		for (int i = 0; !exit_ && !viz.wasStopped(); ++i)
		{
			bool has_frame = capture_.grab(depth, image);
			if (!has_frame)
				return std::cout << "Can't grab" << std::endl, false;

			depth_device_.upload(depth.data, depth.step, depth.rows, depth.cols);

			{
				SampledScopeTime fps(time_ms); (void)fps;
				has_image = kinfu(depth_device_);
			}

			if (has_image)
				show_raycasted(kinfu);

			show_depth(depth);
			//cv::imshow("Image", image);

			if (!iteractive_mode_)
				viz.setViewerPose(kinfu.getCameraPose());

			int key = cv::waitKey(3);

			switch(key)
			{
			case 't': case 'T' : take_cloud(kinfu); break;
			case 'i': case 'I' : iteractive_mode_ = !iteractive_mode_; break;
			case 'a': case 'A' : generate_mesh(kinfu); break;
			case 's': case 'S' : write_mesh(); break;
			case 27: case 32: exit_ = true; break;
			}

			//exit_ = exit_ || i > 100;
			viz.spinOnce(3, true);
		}
		return true;
	}

	bool exit_, iteractive_mode_;
	OpenNISource& capture_;
	KinFu::Ptr kinfu_;
	cv::viz::Viz3d viz;

	cv::Mat view_host_;
	cuda::Image view_device_;
	cuda::Depth depth_device_;
	cuda::DeviceArray<Point> cloud_buffer;

	pcl::PolygonMesh mesh_;
};


///////////////////////////////////////////////////////////////////////////////////////////////////////////////////

int main (int argc, char* argv[])
{
	int device = 0;
	cuda::setDevice (device);
	cuda::printShortCudaDeviceInfo (device);

	if(cuda::checkIfPreFermiGPU(device))
		return std::cout << std::endl << "Kinfu is not supported for pre-Fermi GPU architectures, and not built for them by default. Exiting..." << std::endl, 1;

	OpenNISource capture;
	if (argc == 2) {
		capture.open(argv[1]);
	} 
	else {
		capture.open (0);
	}

	KinFuApp app (capture);

	// executing
	try { app.execute (); }
	catch (const std::bad_alloc& /*e*/) { std::cout << "Bad alloc" << std::endl; }
	catch (const std::exception& /*e*/) { std::cout << "Exception" << std::endl; }

	return 0;
}

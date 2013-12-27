#include "ros/ros.h"
#include <iomanip>
#include <iostream>
#include <dynamic_reconfigure/server.h>
#include "gcop_comm/CtrlTraj.h"//msg for publishing ctrl trajectory
#include <gcop/urdf_parser.h>
#include "tf/transform_datatypes.h"
#include <gcop/se3.h>
#include "gcop/dmoc.h" //gcop dmoc header
#include "gcop/lqcost.h" //gcop lqr header
#include "gcop/rn.h"
#include "gcop/mbscontroller.h"
#include "gcop_ctrl/MbsDMocInterfaceConfig.h"
#include <tf/transform_listener.h>
#include <XmlRpcValue.h>

using namespace std;
using namespace Eigen;
using namespace gcop;

typedef Dmoc<MbsState> MbsDmoc;//defining chaindmoc


//ros messages
gcop_comm::CtrlTraj trajectory;

//Publisher
ros::Publisher trajpub;

//Timer
ros::Timer iteratetimer;

//Subscriber
//ros::Subscriber initialposn_sub;

//Pointer for mbs system
boost::shared_ptr<Mbs> mbsmodel;

//Pointer for Optimal Controller
boost::shared_ptr<MbsDmoc> mbsdmoc;

//MbsState final state
boost::shared_ptr<MbsState> xf;

//Cost lqcost
boost::shared_ptr<LqCost<MbsState>> cost;

boost::shared_ptr<MbsController> ctrl;
int Nit = 10;//number of iterations for dmoc
int N = 100;      // discrete trajectory segments

void q2transform(geometry_msgs::Transform &transformmsg, Vector6d &bpose)
{
	tf::Quaternion q;
	q.setEulerZYX(bpose[2],bpose[1],bpose[0]);
	tf::Vector3 v(bpose[3],bpose[4],bpose[5]);
	tf::Transform tftransform(q,v);
	tf::transformTFToMsg(tftransform,transformmsg);
	//cout<<"---------"<<endl<<transformmsg.position.x<<endl<<transformmsg.position.y<<endl<<transformmsg.position.z<<endl<<endl<<"----------"<<endl;
}

void xml2vec(VectorXd &vec, XmlRpc::XmlRpcValue &my_list)
{
	ROS_ASSERT(my_list.getType() == XmlRpc::XmlRpcValue::TypeArray);
	ROS_ASSERT(my_list.size() > 0);
	vec.resize(my_list.size());
	//cout<<my_list.size()<<endl;
	//ROS_ASSERT(vec.size() <= my_list.size()); //Desired size

	for (int32_t i = 0; i < my_list.size(); i++) 
	{
				ROS_ASSERT(my_list[i].getType() == XmlRpc::XmlRpcValue::TypeDouble);
				cout<<"my_list["<<i<<"]\t"<<my_list[i]<<endl;
			  vec[i] =  (double)(my_list[i]);
	}
}
void pubtraj() //N is the number of segments
{
	int N = mbsdmoc->us.size();
	cout<<"N: "<<N<<endl;
	int nb = mbsmodel->nb;
	cout<<"nb: "<<nb<<endl;
	Vector6d bpose;

	gcop::SE3::Instance().g2q(bpose, mbsdmoc->xs[0].gs[0]);
	q2transform(trajectory.statemsg[0].basepose,bpose);
	trajectory.statemsg[0].statevector.resize(nb-1);
	trajectory.statemsg[0].names.resize(nb-1);

	for(int count1 = 0;count1 < nb-1;count1++)
	{
		trajectory.statemsg[0].statevector[count1] = mbsdmoc->xs[0].r[count1];
		trajectory.statemsg[0].names[count1] = mbsmodel->joints[count1].name;
	}

	for (int i = 0; i < N; ++i) 
	{
		trajectory.statemsg[i+1].statevector.resize(nb-1);
		trajectory.statemsg[i+1].names.resize(nb-1);
		gcop::SE3::Instance().g2q(bpose, mbsdmoc->xs[i+1].gs[0]);
		q2transform(trajectory.statemsg[i+1].basepose,bpose);
		for(int count1 = 0;count1 < nb-1;count1++)
		{
			trajectory.statemsg[i+1].statevector[count1] = mbsdmoc->xs[i+1].r[count1];
			trajectory.statemsg[i+1].names[count1] = mbsmodel->joints[count1].name;
		}
		trajectory.ctrl[i].ctrlvec.resize(6+nb-1);
		for(int count1 = 0;count1 < 6+nb-1;count1++)
		{
			trajectory.ctrl[i].ctrlvec[count1] = mbsdmoc->us[i](count1);
		}
	}
	//final goal:
	gcop::SE3::Instance().g2q(bpose, xf->gs[0]);
	q2transform(trajectory.finalgoal.basepose,bpose);
	trajectory.finalgoal.statevector.resize(nb-1);
	trajectory.finalgoal.names.resize(nb-1);

	for(int count1 = 0;count1 < nb-1;count1++)
	{
		trajectory.finalgoal.statevector[count1] = xf->r[count1];
		trajectory.finalgoal.names[count1] = mbsmodel->joints[count1].name;
	}

	trajpub.publish(trajectory);

}
void iterateCallback(const ros::TimerEvent & event)
{
	//getchar();
	//ros::Time startime = ros::Time::now(); 
	for (int count = 1;count <= Nit;count++)
		mbsdmoc->Iterate();//Updates us and xs after one iteration
	//double te = 1e6*(ros::Time::now() - startime).toSec();
	//cout << "Time taken " << te << " us." << endl;
	//publish the message
	pubtraj();
}
/*
void initialposnCallback(const geometry_msgs::TransformStamped::ConstPtr &currframe)
{
	tf::StampedTransform UV_O;
	transformStampedMsgToTF(*currframe,UV_O);//converts to the right format 
	//getrpy:

	double roll,pitch,yaw;
	UV_O.getBasis().getRPY(roll,pitch,yaw);
	double tcurr = currframe->header.stamp.toSec();
	tf::Vector3 y = UV_O.getOrigin();
	Vector4d x0 = Vector4d::Zero();// initial state
	x0[0] = y[0];
	x0[1] = y[1];
	x0[2] = yaw;
	x0[3] =0;/// need to calculate velocity
	xs[0] = x0;
	//ros::TimerEvent e1;
	//iterateCallback(e1);
	return;
}
*/
void paramreqcallback(gcop_ctrl::MbsDMocInterfaceConfig &config, uint32_t level) 
{
	int nb = mbsmodel->nb;
	Nit = config.Nit; 
	//int N = config.N;
	double h = config.tf/N;   // time step

	if(level & 0x00000001)
	{
		Vector3d rpy;
		Vector3d xyz;

		ROS_ASSERT((config.i_Q <= mbsmodel->X.n)&& (config.i_Q > 0));
		config.Qfi = cost->Qf(config.i_Q -1,config.i_Q -1);
		config.Qi = cost->Q(config.i_Q -1,config.i_Q -1);

		ROS_ASSERT((config.i_R <= mbsmodel->U.n)&& (config.i_R > 0));
		config.Ri = cost->R(config.i_R -1,config.i_R -1);

		if(config.final)
		{
			// overwrite the config with values from final state
			config.vroll = xf->vs[0](0);
			config.vpitch = xf->vs[0](1);
			config.vyaw = xf->vs[0](2);
			config.vx = xf->vs[0](3);
			config.vy = xf->vs[0](4);
			config.vz = xf->vs[0](5);

			gcop::SE3::Instance().g2rpyxyz(rpy,xyz,xf->gs[0]);
			config.roll = rpy(0);
			config.pitch = rpy(1);
			config.yaw = rpy(2);
			config.x = xyz(0);
			config.y = xyz(1);
			config.z = xyz(2);

			ROS_ASSERT((config.i_J <= nb) && (config.i_J > 0));
			config.Ji = xf->r[config.i_J-1];     
		}
		else
		{
			// overwrite the config with values from initial state
			config.vroll = mbsdmoc->xs[0].vs[0](0);
			config.vpitch = mbsdmoc->xs[0].vs[0](1);
			config.vyaw = mbsdmoc->xs[0].vs[0](2);
			config.vx = mbsdmoc->xs[0].vs[0](3);
			config.vy = mbsdmoc->xs[0].vs[0](4);
			config.vz = mbsdmoc->xs[0].vs[0](5);

			gcop::SE3::Instance().g2rpyxyz(rpy,xyz,mbsdmoc->xs[0].gs[0]);
			config.roll = rpy(0);
			config.pitch = rpy(1);
			config.yaw = rpy(2);
			config.x = xyz(0);
			config.y = xyz(1);
			config.z = xyz(2);

			ROS_ASSERT((config.i_J <= nb) && (config.i_J > 0));
			config.Ji = mbsdmoc->xs[0].r[config.i_J-1];     
		}
		return;
	}

	if(config.final)
	{
		gcop::SE3::Instance().rpyxyz2g(xf->gs[0], Vector3d(config.roll,config.pitch,config.yaw), Vector3d(config.x,config.y,config.z));
		xf->vs[0]<<config.vroll, config.vpitch, config.vyaw, config.vx, config.vy, config.vz;
		ROS_ASSERT((config.i_J <= nb)&&(config.i_J > 0));
		xf->r[config.i_J-1] = config.Ji;
		xf->dr[config.i_J-1] = config.Jvi;
	}
	else
	{
		gcop::SE3::Instance().rpyxyz2g(mbsdmoc->xs[0].gs[0], Vector3d(config.roll,config.pitch,config.yaw), Vector3d(config.x,config.y,config.z));
		mbsdmoc->xs[0].vs[0]<<config.vroll, config.vpitch, config.vyaw, config.vx, config.vy, config.vz;
		ROS_ASSERT((config.i_J <= nb)&&(config.i_J > 0));
		mbsdmoc->xs[0].r[config.i_J -1] = config.Ji;
		mbsdmoc->xs[0].dr[config.i_J -1] = config.Jvi;
		mbsmodel->Rec(mbsdmoc->xs[0], h);
	}

	ROS_ASSERT((config.i_Q <= mbsmodel->X.n)&& (config.i_Q > 0));
	cost->Qf(config.i_Q -1,config.i_Q -1) = config.Qfi;
	cost->Q(config.i_Q -1,config.i_Q -1) = config.Qi;

	ROS_ASSERT((config.i_R <= mbsmodel->U.n)&& (config.i_R > 0));
	cost->R(config.i_R -1,config.i_R -1) = config.Ri;

	//resize
	//mbsdmoc->xs.resize(N+1);
	//mbsdmoc->us.resize(N);
	//mbsdmoc->ts.resize(N+1);
	//trajectory.N = N;
	//trajectory.statemsg.resize(N+1);
	//trajectory.ctrl.resize(N);

 //Setting Values
	//for (int k = 0; k <=N; ++k)
	//	mbsdmoc->ts[k] = k*h;

	/*VectorXd u(6+ (nb-1));
	u.setZero();
	for(int count = 0;count < nb;count++)
		u[5] += (mbsmodel->links[count].m)*(-mbsmodel->ag(2));
		*/

	for (int i = 0; i < mbsdmoc->xs.size()-1; ++i) 
	{
		double t = i*h;
		ctrl->Set(mbsdmoc->us[i], t, mbsdmoc->xs[i]);
		mbsmodel->Step(mbsdmoc->xs[i+1], i*h, mbsdmoc->xs[i], mbsdmoc->us[i], h);
	}

	mbsdmoc->mu = config.mu;
}


int main(int argc, char** argv)
{
	ros::init(argc, argv, "chainload");
	ros::NodeHandle n("mbsdmoc");
	//Initialize publisher
	trajpub = n.advertise<gcop_comm::CtrlTraj>("ctrltraj",2);
	//Subscribe to initial posn from tf
	//initialposn_sub = rosdmoc.subscribe("mocap",1,initialposnCallback);
	//get parameter for xml_string:
	string xml_string, xml_filename;
	if(!ros::param::get("/robot_description", xml_string))
	{
		ROS_ERROR("Could not fetch xml file name");
		return 0;
	}
	VectorXd xmlconversion;
	//Create Mbs system
	mbsmodel = gcop_urdf::mbsgenerator(xml_string);
	mbsmodel->ag << 0, 0, -0.05;
	//get ag from parameters
	XmlRpc::XmlRpcValue ag_list;
	if(n.getParam("ag", ag_list))
		xml2vec(xmlconversion,ag_list);
	ROS_ASSERT(xmlconversion.size() == 3);
	mbsmodel->ag = xmlconversion.head(3);

	//Printing the mbsmodel params:
	for(int count = 0;count<(mbsmodel->nb);count++)
	{
		cout<<"Ds["<<mbsmodel->links[count].name<<"]"<<endl<<mbsmodel->links[count].ds<<endl;
		cout<<"I["<<mbsmodel->links[count].name<<"]"<<endl<<mbsmodel->links[count].I<<endl;
	}
	for(int count = 0;count<(mbsmodel->nb)-1;count++)
	{
		cout<<"Joint["<<mbsmodel->joints[count].name<<"].gc"<<endl<<mbsmodel->joints[count].gc<<endl;
		cout<<"Joint["<<mbsmodel->joints[count].name<<"].gp"<<endl<<mbsmodel->joints[count].gp<<endl;
		cout<<"Joint["<<mbsmodel->joints[count].name<<"].a"<<endl<<mbsmodel->joints[count].a<<endl;
	}

	//Using it:
	//define parameters for the system
	int nb = mbsmodel->nb;
	double tf = 20;   // time-horizon

	n.getParam("tf",tf);
	n.getParam("N",N);

	double h = tf/N; // time-step

	

	//times
	vector<double> ts(N+1);
	for (int k = 0; k <=N; ++k)
		ts[k] = k*h;


	//Define Final State
	xf.reset( new MbsState(nb));
	xf->gs[0].setIdentity();
	xf->vs[0].setZero();
	xf->dr.setZero();
	xf->r.setZero();

	// Get Xf	 from params
	XmlRpc::XmlRpcValue xf_list;
	if(n.getParam("XN", xf_list))
		xml2vec(xmlconversion,xf_list);
	ROS_ASSERT(xmlconversion.size() == 12);
	xf->vs[0] = xmlconversion.tail<6>();
  gcop::SE3::Instance().rpyxyz2g(xf->gs[0],xmlconversion.head<3>(),xmlconversion.segment<3>(3)); 
  //list of joint angles:
	XmlRpc::XmlRpcValue xfj_list;
	if(n.getParam("JN", xfj_list))
		xml2vec(xf->r,xfj_list);
	cout<<"xf->r"<<endl<<xf->r<<endl;


	//Define Lqr Cost
	cost.reset(new LqCost<MbsState>(mbsmodel->X, (Rn<>&)mbsmodel->U, tf, *xf));
	cost->Qf(0,0) = 2; cost->Qf(1,1) = 2; cost->Qf(2,2) = 2;
	cost->Qf(3,3) = 20; cost->Qf(4,4) = 20; cost->Qf(5,5) = 20;
	//cost.Qf(9,9) = 20; cost.Qf(10,10) = 20; cost.Qf(11,11) = 20;
	//list of final cost :
	XmlRpc::XmlRpcValue finalcost_list;
	if(n.getParam("Qf", finalcost_list))
	{
		cout<<mbsmodel->X.n<<endl;
		xml2vec(xmlconversion,finalcost_list);
		cout<<"conversion"<<endl<<xmlconversion<<endl;
		ROS_ASSERT(xmlconversion.size() == mbsmodel->X.n);
		cost->Qf = xmlconversion.asDiagonal();
		cout<<"Cost.Qf"<<endl<<cost->Qf<<endl;
	}
//
	XmlRpc::XmlRpcValue statecost_list;
	if(n.getParam("Q", statecost_list))
	{
		cout<<mbsmodel->X.n<<endl;
		xml2vec(xmlconversion,statecost_list);
		cout<<"conversion"<<endl<<xmlconversion<<endl;
		ROS_ASSERT(xmlconversion.size() == mbsmodel->X.n);
		cost->Q = xmlconversion.asDiagonal();
		cout<<"Cost.Q"<<endl<<cost->Q<<endl;
	}
	XmlRpc::XmlRpcValue ctrlcost_list;
	if(n.getParam("R", ctrlcost_list))
	{
		cout<<mbsmodel->U.n<<endl;
		xml2vec(xmlconversion,ctrlcost_list);
		ROS_ASSERT(xmlconversion.size() == mbsmodel->U.n);
		cout<<"conversion"<<endl<<xmlconversion<<endl;
		cost->R = xmlconversion.asDiagonal();
	}
		cout<<"Cost.R"<<endl<<cost->R<<endl;
//


	//Define the initial state mbs
	MbsState x(nb);
	x.gs[0].setIdentity();
	x.vs[0].setZero();
	x.dr.setZero();
	x.r.setZero();

	// Get X0	 from params
	XmlRpc::XmlRpcValue x0_list;
	if(n.getParam("X0", x0_list))
		xml2vec(xmlconversion,x0_list);
	ROS_ASSERT(xmlconversion.size() == 12);
	x.vs[0] = xmlconversion.tail<6>();
  gcop::SE3::Instance().rpyxyz2g(x.gs[0],xmlconversion.head<3>(),xmlconversion.segment<3>(3)); 
  //list of joint angles:
	XmlRpc::XmlRpcValue j_list;
	if(n.getParam("J0", j_list))
		xml2vec(x.r,j_list);
	cout<<"x.r"<<endl<<x.r<<endl;
	mbsmodel->Rec(x, h);

	// initial controls (e.g. hover at one place)
	VectorXd u(6+ (nb-1));
	u.setZero();
	for(int count = 0;count < nb;count++)
		u[5] += (mbsmodel->links[count].m)*(-mbsmodel->ag(2));

	cout<<"u[5]: "<<u[5]<<endl;
	//n.getParam("ua",u[5]);


	//States and controls for system
	vector<VectorXd> us(N,u);
	vector<MbsState> xs(N+1,x);

	// @MK: this is the new part, initialize trajectory using a controller
  ctrl.reset(new MbsController(*mbsmodel, xf.get()));

	XmlRpc::XmlRpcValue kp_list;
	if(n.getParam("Kp", kp_list))
	xml2vec(xmlconversion,kp_list);
	ROS_ASSERT(xmlconversion.size() == 6+nb-1);
	ctrl->Kp = xmlconversion.head(6+nb-1);
	cout<<"Kp"<<endl<<ctrl->Kp<<endl;

	XmlRpc::XmlRpcValue kd_list;
	if(n.getParam("Kd", kd_list))
	xml2vec(xmlconversion,kd_list);
	ROS_ASSERT(xmlconversion.size() == 6+nb-1);
	ctrl->Kd = xmlconversion.head(6+nb-1);
	cout<<"Kd"<<endl<<ctrl->Kd<<endl;


  for (int i = 0; i < xs.size()-1; ++i) {
    double t = i*h;
    ctrl->Set(us[i], t, xs[i]); 
    mbsmodel->Step(xs[i+1], i*h, xs[i], us[i], h);
  }
	//cout<<"us"<<endl<<us<<endl;


  // see the result before running optimization
//  getchar();


	mbsdmoc.reset(new MbsDmoc(*mbsmodel, *cost, ts, xs, us));
	mbsdmoc->mu = 0.001;

	//Trajectory message initialization
	trajectory.N = N;
	trajectory.statemsg.resize(N+1);
	trajectory.ctrl.resize(N);
	trajectory.time = ts;
	trajectory.finalgoal.statevector.resize(nb-1);
	
	//Debug true for mbs

  //mbsmodel->debug = true;

	
	// Get number of iterations
	n.getParam("Nit",Nit);
	// Create timer for iterating	
	iteratetimer = n.createTimer(ros::Duration(1), iterateCallback);
	iteratetimer.start();
	//Dynamic Reconfigure setup Callback ! immediately gets called with default values	
	dynamic_reconfigure::Server<gcop_ctrl::MbsDMocInterfaceConfig> server;
	dynamic_reconfigure::Server<gcop_ctrl::MbsDMocInterfaceConfig>::CallbackType f;
	f = boost::bind(&paramreqcallback, _1, _2);
	server.setCallback(f);
	ros::spin();
	return 0;
}


	





#include "ControlUtil.h"
#include <algorithm>
#include <limits>
#include <cmath>

using namespace std;

typedef Matrix<double, 6,1> Vector6d;

struct PelvisMotionControlData {
  RigidBodyManipulator* r;
  double alpha;
  double pelvis_height_previous;
  double nominal_pelvis_height;
  Vector6d Kp;
  Vector6d Kd;

  int pelvis_body_index;
  int rfoot_body_index;
  int lfoot_body_index;
};

// TODO: remove me---stick me in controlUtil in drake and templetize

template <typename DerivedPhi1, typename DerivedPhi2, typename DerivedD>
void angleDiff(const MatrixBase<DerivedPhi1>& phi1, const MatrixBase<DerivedPhi2>& phi2, MatrixBase<DerivedD>& d) {
  d = phi2 - phi1;
  
  for (int i = 0; i < phi1.rows(); i++) {
    for (int j = 0; j < phi1.cols(); j++) {
      if (d(i,j) < -M_PI) {
        d(i,j) = fmod(d(i,j) + M_PI, 2*M_PI) + M_PI;
      } else {
        d(i,j) = fmod(d(i,j) + M_PI, 2*M_PI) - M_PI;
      }
    }
  }
}



void mexFunction(int nlhs, mxArray *plhs[], int nrhs, const mxArray *prhs[])
{
  if (nrhs<1) mexErrMsgTxt("usage: ptr = pelvisMotionControlmex(0,robot_obj,alpha,nominal_pelvis_height,Kp,Kd); y=pelvisMotionControlmex(ptr,x)");
  if (nlhs<1) mexErrMsgTxt("take at least one output... please.");
  
  struct PelvisMotionControlData* pdata;

  if (mxGetScalar(prhs[0])==0) { // then construct the data object and return
    pdata = new struct PelvisMotionControlData;
    
    // get robot mex model ptr
    if (!mxIsNumeric(prhs[1]) || mxGetNumberOfElements(prhs[1])!=1)
      mexErrMsgIdAndTxt("DRC:pelvisMotionControlmex:BadInputs","the second argument should be the robot mex ptr");
    memcpy(&(pdata->r),mxGetData(prhs[1]),sizeof(pdata->r));
        
    if (!mxIsNumeric(prhs[2]) || mxGetNumberOfElements(prhs[2])!=1)
    mexErrMsgIdAndTxt("DRC:pelvisMotionControlmex:BadInputs","the third argument should be alpha");
    memcpy(&(pdata->alpha),mxGetPr(prhs[2]),sizeof(pdata->alpha));

    if (!mxIsNumeric(prhs[3]) || mxGetNumberOfElements(prhs[3])!=1)
    mexErrMsgIdAndTxt("DRC:pelvisMotionControlmex:BadInputs","the fourth argument should be nominal_pelvis_height");
    memcpy(&(pdata->nominal_pelvis_height),mxGetPr(prhs[3]),sizeof(pdata->nominal_pelvis_height));

    if (!mxIsNumeric(prhs[4]) || mxGetM(prhs[4])!=6 || mxGetN(prhs[4])!=1)
    mexErrMsgIdAndTxt("DRC:pelvisMotionControlmex:BadInputs","the fifth argument should be Kp");
    memcpy(&(pdata->Kp),mxGetPr(prhs[4]),sizeof(pdata->Kp));

    if (!mxIsNumeric(prhs[5]) || mxGetM(prhs[5])!=6 || mxGetN(prhs[5])!=1)
    mexErrMsgIdAndTxt("DRC:pelvisMotionControlmex:BadInputs","the sixth argument should be Kd");
    memcpy(&(pdata->Kd),mxGetPr(prhs[5]),sizeof(pdata->Kd));

    
    mxClassID cid;
    if (sizeof(pdata)==4) cid = mxUINT32_CLASS;
    else if (sizeof(pdata)==8) cid = mxUINT64_CLASS;
    else mexErrMsgIdAndTxt("Drake:pelvisMotionControlmex:PointerSize","Are you on a 32-bit machine or 64-bit machine??");
     
    pdata->pelvis_height_previous = -1;

    pdata->pelvis_body_index = pdata->r->findLinkInd("pelvis", 0);
    pdata->rfoot_body_index = pdata->r->findLinkInd("r_foot", 0);
    pdata->lfoot_body_index = pdata->r->findLinkInd("l_foot", 0);

    plhs[0] = mxCreateNumericMatrix(1,1,cid,mxREAL);
    memcpy(mxGetData(plhs[0]),&pdata,sizeof(pdata));
     
    return;
  }
  
  // first get the ptr back from matlab
  if (!mxIsNumeric(prhs[0]) || mxGetNumberOfElements(prhs[0])!=1)
    mexErrMsgIdAndTxt("DRC:pelvisMotionControlmex:BadInputs","the first argument should be the ptr");
  memcpy(&pdata,mxGetData(prhs[0]),sizeof(pdata));

  int nq = pdata->r->num_dof;

  double *q = mxGetPr(prhs[1]);
  double *qd = &q[nq];
  Map<VectorXd> qdvec(qd,nq);

  pdata->r->doKinematics(q,false,qd);

  // TODO: this must be updated to use quaternions/spatial velocity
  Vector6d pelvis_pose,rfoot_pose,lfoot_pose;
  MatrixXd Jpelvis = MatrixXd::Zero(6,pdata->r->num_dof);
  Vector4d zero = Vector4d::Zero();
  zero(3) = 1.0;
  pdata->r->forwardKin(pdata->pelvis_body_index,zero,1,pelvis_pose);
  pdata->r->forwardJac(pdata->pelvis_body_index,zero,1,Jpelvis);
  pdata->r->forwardKin(pdata->rfoot_body_index,zero,1,rfoot_pose);
  pdata->r->forwardKin(pdata->lfoot_body_index,zero,1,lfoot_pose);

  if (pdata->pelvis_height_previous<0) {
    pdata->pelvis_height_previous = pelvis_pose(2);
  }

  double min_foot_z = std::min(lfoot_pose(2),rfoot_pose(2));
  double mean_foot_yaw = (lfoot_pose(5)+rfoot_pose(5))/2.0;

  double pelvis_height_desired = pdata->alpha*pdata->pelvis_height_previous + (1.0-pdata->alpha)*(min_foot_z + pdata->nominal_pelvis_height); 
  pdata->pelvis_height_previous = pelvis_height_desired;
      
  Vector6d body_des;
  double nan = std::numeric_limits<double>::quiet_NaN();
  body_des << nan,nan,pelvis_height_desired,0,0,mean_foot_yaw; 
  Vector6d error;
  error.head<3>()= body_des.head<3>()-pelvis_pose.head<3>();

  Vector3d error_rpy;
  angleDiff(pelvis_pose.tail<3>(),body_des.tail<3>(),error_rpy);
  error.tail(3) = error_rpy;

  Matrix<double,6,1> body_vdot = (pdata->Kp.array()*error.array()).matrix() - (pdata->Kd.array()*(Jpelvis*qdvec).array()).matrix();
  
  plhs[0] = eigenToMatlab(body_vdot);
}
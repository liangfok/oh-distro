options.floating = true;
options.dt = 0.001;
% r = Atlas('../../models/mit_gazebo_models/mit_robot_drake/model_minimal_contact.urdf', options);
r = Atlas('../drake/examples/Atlas/urdf/atlas_minimal_contact.urdf', options);
d = load('../data/atlas_fp.mat');
xstar = d.xstar;
r = r.setInitialState(xstar);
biped = Biped(r);

% poses = [[0;1;0;0;0;pi/2], [1;1;0;0;0;0], [1;0;0;0;0;0]];
poses = [[0;1;0;0;0;pi/2], [1;1;0;0;0;0]];
xtraj = biped.roughWalkingPlan(xstar, poses, struct('plotting', true, ...
  'interactive', false, 'traj_type', 'turn_and_go'));
biped.visualizer.playback(xtraj, struct('slider', true));

% % poses = [1;1;0;0;0;-pi/4];
% poses = [1.5;0;0;0;0;0];
% xtraj = biped.roughWalkingPlan(xstar, poses, struct('plotting', true, ...
%   'interactive', true, 'traj_type', 'cubic_spline'));

biped.visualizer.playback(xtraj, struct('slider', true));
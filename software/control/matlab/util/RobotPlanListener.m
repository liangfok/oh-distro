classdef RobotPlanListener
	properties
		lc
		aggregator
		floating
	end

	methods
		function obj = RobotPlanListener(channel,floating)
			obj.lc = lcm.lcm.LCM.getSingleton();
			obj.aggregator = lcm.lcm.MessageAggregator();
			obj.lc.subscribe(channel, obj.aggregator);
			if nargin > 1
				obj.floating = floating;
			else
				obj.floating = true;
			end
		end

		function [X,T] = getNextMessage(obj, t_ms)
			plan_msg = obj.aggregator.getNextMessage(t_ms);
			if isempty(plan_msg)
				X = [];T=[];
			else
				[X,T] = RobotPlanListener.decodeRobotPlan(drc.robot_plan_t(plan_msg.data),obj.floating);
			end
		end

	end

	methods(Static)
		function [X,T] = decodeRobotPlan(msg,floating)
			
			float_offset = 0;
			if floating
				float_offset = 6;
			end

			num_dofs = float_offset + msg.plan(1).num_joints; 
			X = zeros(num_dofs*2,msg.num_states);
      T= zeros(1,msg.num_states);
			for i=1:msg.num_states
			    T(i) = double(msg.plan(i).utime)/1000000;; % use relative time index. At the time of plan execution, this will be offset by latest sim time est from the robot state msg.
        if floating
					X(1,i) = msg.plan(i).origin_position.translation.x;
					X(2,i) = msg.plan(i).origin_position.translation.y;
					X(3,i) = msg.plan(i).origin_position.translation.z;
        	X(num_dofs+1,i) = msg.plan(i).origin_twist.linear_velocity.x;
        	X(num_dofs+2,i) = msg.plan(i).origin_twist.linear_velocity.y;
        	X(num_dofs+3,i) = msg.plan(i).origin_twist.linear_velocity.z;
      
					rpy = quat2rpy([msg.plan(i).origin_position.rotation.w,...
	                        msg.plan(i).origin_position.rotation.x,...
	                        msg.plan(i).origin_position.rotation.y,...
	                        msg.plan(i).origin_position.rotation.z]);

          X(4,i) = rpy(1);
          X(5,i) = rpy(2);
          X(6,i) = rpy(3);
					
					X(num_dofs+4,i) = msg.plan(i).origin_twist.angular_velocity.x;
					X(num_dofs+5,i) = msg.plan(i).origin_twist.angular_velocity.y;
					X(num_dofs+6,i) = msg.plan(i).origin_twist.angular_velocity.z;
        end  
        
        for j=float_offset+1:num_dofs
          X(j,i) = msg.plan(i).joint_position(j-float_offset);
          X(j+num_dofs,i) = msg.plan(i).joint_velocity(j-float_offset);
        end
			end
			
		end
	end
end

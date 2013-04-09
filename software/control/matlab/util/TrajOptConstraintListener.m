classdef TrajOptConstraintListener
    properties
        lc
        aggregator
    end
    
    methods
        function obj = TrajOptConstraintListener(channel)
            obj.lc = lcm.lcm.LCM.getSingleton();
            obj.aggregator = lcm.lcm.MessageAggregator();
            obj.lc.subscribe(channel, obj.aggregator);
        end
        
        function X = getNextMessage(obj, t_ms)
            msg_raw = obj.aggregator.getNextMessage(t_ms);
            
            if isempty(msg_raw)
                X = [];
            else
                msg = drc.traj_opt_constraint_t(msg_raw.data);
                X = TrajOptConstraintListener.decodeTrajOptConstraint(msg);
            end
        end
        
    end
    
    methods(Static)
        function X = decodeTrajOptConstraint(msg)
            X = [];
            
            for j = 1:msg.num_links,
                X(j).name = msg.link_name(j);
                X(j).time = double(msg.link_timestamps(j))/1000000;
                X(j).desired_pose = [msg.link_origin_position(j).translation.x;
                                    msg.link_origin_position(j).translation.y;
                                    msg.link_origin_position(j).translation.z;
                                    msg.link_origin_position(j).rotation.w;
                                    msg.link_origin_position(j).rotation.x;
                                    msg.link_origin_position(j).rotation.y;
                                    msg.link_origin_position(j).rotation.z];

            end
        end
    end
end

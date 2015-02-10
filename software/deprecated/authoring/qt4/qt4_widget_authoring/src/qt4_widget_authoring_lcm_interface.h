#ifndef AUTHORING_QT4_WIDGET_AUTHORING_LCM_INTERFACE_H
#define AUTHORING_QT4_WIDGET_AUTHORING_LCM_INTERFACE_H

#include <iostream>
#include <QtGui/QWidget>
#include <QtGui/QMainWindow>

#include <lcm/lcm-cpp.hpp>
#include <lcmtypes/bot_core.hpp>
#include <lcmtypes/drc_lcmtypes.hpp>

#include <affordance/AffordanceState.h>

#include <state/state_gfe.h>

#include <authoring/qt4_widget_authoring.h>

namespace authoring {
  class Qt4_Widget_Authoring_LCM_Interface : public QMainWindow {
    Q_OBJECT
  public:
    Qt4_Widget_Authoring_LCM_Interface( const std::string& xmlString, QWidget * parent = 0 );
    ~Qt4_Widget_Authoring_LCM_Interface();
    Qt4_Widget_Authoring_LCM_Interface( const Qt4_Widget_Authoring_LCM_Interface& other );
    Qt4_Widget_Authoring_LCM_Interface& operator=( const Qt4_Widget_Authoring_LCM_Interface& other );

  signals:
    void affordance_collection_update( std::vector< affordance::AffordanceState >& affordanceCollection );
    void state_gfe_update( state::State_GFE& stateGFE );
    void robot_plan_update( const std::vector< state::State_GFE >& robotPlan );
    void robot_plan_insert( const state::State_GFE& robotPlanSlice );
    void aas_got_status_msg( long int plan_index, bool server_ready_status, 
        float last_time_solved, float total_time_to_solve, bool solving_highres, 
        bool plan_is_good, bool plan_is_warn );

  public slots:
    void publish_drc_action_sequence_t( const drc::action_sequence_t& msg );
    void publish_robot_plan_w_keyframes_t( const drc::robot_plan_w_keyframes_t& msg );
    void publish_drc_action_sequence_t_oneshot( const drc::action_sequence_t& msg );

  protected slots:
    void _handle_lcm_timer_timeout( void );
    void _handle_est_robot_state_msg( const lcm::ReceiveBuffer* rbuf, const std::string& channel, const drc::robot_state_t* msg );
    void _handle_affordance_collection_msg( const lcm::ReceiveBuffer* rbuf, const std::string& channel, const drc::affordance_collection_t* msg );
    void _handle_candidate_robot_plan_msg( const lcm::ReceiveBuffer* rbuf, const std::string& channel, const drc::robot_plan_t* msg );
    void _handle_action_authoring_server_status_msg( const lcm::ReceiveBuffer* rbuf, const std::string& channel, const drc::action_authoring_server_status_t* msg );
    void _handle_single_shot_msg( const lcm::ReceiveBuffer* rbuf, const std::string& channel, const drc::robot_plan_t* msg );

  protected:
    lcm::LCM *  _lcm;
    QTimer *    _lcm_timer;
    Qt4_Widget_Authoring * _qt4_widget_authoring;

  private:

  };
  std::ostream& operator<<( std::ostream& out, const Qt4_Widget_Authoring_LCM_Interface& other );
}

#endif /* AUTHORING_QT4_WIDGET_AUTHORING_LCM_INTERFACE_H */
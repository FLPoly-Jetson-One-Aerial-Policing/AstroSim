#pragma once
#include <gz/msgs/odometry.pb.h>
#include <gz/transport/Node.hh>

#include <atomic>
#include <mutex>
#include <string>

#include "gateway.hpp"

namespace astrosim {

class GzLink : public GzSide {
 public:
  explicit GzLink(const std::string& model = "X3");
  bool connected() override;
  StateMsg state() override;
  void command(const VelCmd&) override;
  void enable() override;

 private:
  void onOdom(const gz::msgs::Odometry&);

  gz::transport::Node node_;
  gz::transport::Node::Publisher twist_pub_, enable_pub_;
  std::mutex mu_;
  StateMsg state_;
  std::atomic<double> last_pose_wall_{0};
};

}  // namespace astrosim

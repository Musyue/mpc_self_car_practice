//
// Created by yue on 15/3/2022.
//

#include <math.h>
#include <uWS/uWS.h>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>
#include <algorithm>
#include "../third_party/Eigen-3.3/Eigen/Core"
#include "../third_party/Eigen-3.3/Eigen/QR"
#include "../include/mpc.h"
#include "../include/json.hpp"
//for draw
#include <fstream>
#include <map>
#include <limits>
#include <cmath>
#include <cstdio>
#include "../third_party/gnuplot-iostream/gnuplot-iostream.h"
// for convenience
using json = nlohmann::json;

// For converting back and forth between radians and degrees.
constexpr double pi() { return M_PI; }
double deg2rad(double x) { return x * pi() / 180; }
double rad2deg(double x) { return x * 180 / pi(); }

// Checks if the SocketIO event has JSON data.
// If there is data the JSON object in string format will be returned,
// else the empty string "" will be returned.
//用于解析json，和仿真器通信
std::string hasData(std::string s) {
    auto found_null = s.find("null");
    auto b1 = s.find_first_of("[");
    auto b2 = s.rfind("}]");
    if (found_null != std::string::npos) {
        return "";
    } else if (b1 != std::string::npos && b2 != std::string::npos) {
        return s.substr(b1, b2 - b1 + 2);
    }
    return "";
}


//计算多项式的值
double polyeval(Eigen::VectorXd coeffs, double x) {
    double result = 0.0;
    for (int i = 0; i < coeffs.size(); i++) {
        result += coeffs[i] * pow(x, i);
    }
    return result;
}

//拟合一条曲线
// Fit a polynomial.
// Adapted from
// https://github.com/JuliaMath/Polynomials.jl/blob/master/src/Polynomials.jl#L676-L716
Eigen::VectorXd polyfit(Eigen::VectorXd xvals, Eigen::VectorXd yvals,
                        int order) {
    assert(xvals.size() == yvals.size());
    assert(order >= 1 && order <= xvals.size() - 1);
    Eigen::MatrixXd A(xvals.size(), order + 1);

    for (int i = 0; i < xvals.size(); i++) {
        A(i, 0) = 1.0;
    }

    for (int j = 0; j < xvals.size(); j++) {
        for (int i = 0; i < order; i++) {
            A(j, i + 1) = A(j, i) * xvals(j);
        }
    }

    auto Q = A.householderQr();
    auto result = Q.solve(yvals);
    return result;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cout<<"Please choose open(o) draw or close(c):"<<std::endl;
        return EXIT_FAILURE;
    }
    bool open_draw= true;
    if (argv[1][0]=='c') {
        open_draw= false;
    }
    uWS::Hub h;
    // MPC is initialized here!
    mpc mpc;
    int count_for_obj=0;
    Gnuplot gp;
    const int N = 1000;
    std::vector<double> pts(N);
    gp << "set yrange [0:1e4]\n";
    h.onMessage([&gp,&pts,&count_for_obj,&mpc,&open_draw](uWS::WebSocket<uWS::SERVER> ws, char *data, size_t length,
                       uWS::OpCode opCode) {
        // "42" at the start of the message means there's a websocket message event.
        // The 4 signifies a websocket message
        // The 2 signifies a websocket event
        //官方模板，不用管
        std::string sdata = std::string(data).substr(0, length);
//        std::cout << sdata << std::endl;
        if (sdata.size() > 2 && sdata[0] == '4' && sdata[1] == '2') {
            std::string s = hasData(sdata);
            if (s != "") {
                auto j = json::parse(s);
                std::string event = j[0].get<std::string>();
                if (event == "telemetry") { // 数据解析
                    // j[1] is the data JSON object
                    //这个部分也是官方返回的数据，也就是平台返回的数据。
                    std::vector<double> ptsx = j[1]["ptsx"]; //世界系追踪轨迹的位置
                    std::vector<double> ptsy = j[1]["ptsy"];
                    double px = j[1]["x"];//世界系下
                    double py = j[1]["y"];
                    double psi = j[1]["psi"];
                    double v = j[1]["speed"];
                    double delta = j[1]["steering_angle"];
                    double acceleration = j[1]["throttle"];

                    double latency = 0.1; // 100 ms 计算MPC频率
                    const double L = 2.67;
                    px = px + (v)*cos(psi)*latency; //世界坐标系下
                    py = py + (v)*sin(psi)*latency;
                    psi = psi - v/L*delta*latency;
                    v = v + acceleration*latency;
                    //这里需要把轨迹坐标从世界系转换为车体坐标系
                    for (int i = 0; i < (int)ptsx.size(); ++i)
                    {
                        auto xdiff = ptsx[i] - px;
                        auto ydiff = ptsy[i] - py;

                        ptsx[i] = xdiff * cos(psi) + ydiff * sin(psi);
                        ptsy[i] = ydiff * cos(psi) - xdiff * sin(psi);
                    }

                    // calculate coeffs of reference trajectory polynomial
                    //计算系数矩阵
                    Eigen::Map<Eigen::VectorXd> ptsxeig(&ptsx[0], ptsx.size());
                    Eigen::Map<Eigen::VectorXd> ptsyeig(&ptsy[0], ptsy.size());
                    auto coeffs = polyfit(ptsxeig, ptsyeig, 3);

                    // calculate the cross track error
                    double cte = polyeval(coeffs, 0);
                    // calculate the orientation error
                    // f(x) is the polynomial defining the reference trajectory
                    // f'(x) = 3Ax^2 + 2Bx + C
                    // double f_prime_x = 3*coeffs[3]*pow(px,2) + 2*coeffs[2]*px + coeffs[1];
                    double f_prime_x = coeffs[1];
                    double epsi = -atan(f_prime_x);

                    // state
                    Eigen::VectorXd state(6);
                    state << 0, 0, 0, v, cte, epsi;

                    // solve mpc for state and reference trajectory
                    // returns [steering_angle, acceleration]
                    auto actuations = mpc.Solve(state, coeffs);

                    double steer_value = actuations[0]/deg2rad(25); // normalize between [-1,1]
                    double throttle_value = actuations[1];

                    json msgJson;
                    msgJson["steering_angle"] = steer_value;
                    msgJson["throttle"] = throttle_value;
                    std::cout<<"steer_value "<<steer_value<<"throttle_value "<<throttle_value<<std::endl;
                    //Display the MPC predicted trajectory
                    //.. add (x,y) points to list here, points are in reference to the vehicle's coordinate system
                    // the points in the simulator are connected by a Green line

                    msgJson["mpc_x"] = mpc.x_pred_vals;
                    msgJson["mpc_y"] = mpc.y_pred_vals;

                    //Display the waypoints/reference line
                    //.. add (x,y) points to list here, points are in reference to the vehicle's coordinate system
                    // the points in the simulator are connected by a Yellow line
                    std::vector<double> next_x_vals;
                    std::vector<double> next_y_vals;
                    int n_waypoints = 25;
                    int step = 2.5;
                    for (int i = 1; i<n_waypoints; ++i)
                    {
                        next_x_vals.push_back(step*i);
                        next_y_vals.push_back(polyeval(coeffs, step*i));
                    }
                    msgJson["next_x"] = next_x_vals;
                    msgJson["next_y"] = next_y_vals;
                    auto msg = "42[\"steer\"," + msgJson.dump() + "]";
//                    std::cout << msg << std::endl;
                    // Latency
                    // The purpose is to mimic real driving conditions where
                    // the car does actuate the commands instantly.
                    //
                    // Feel free to play around with this value but should be to drive
                    // around the track with 100ms latency.
                    //
                    // NOTE: REMEMBER TO SET THIS TO 100 MILLISECONDS BEFORE
                    // SUBMITTING.
                    //draw the figure
                    if (open_draw) {
                        pts[count_for_obj] = mpc.object_value_out;
                        gp << "plot '-' binary" << gp.binFmt1d(pts, "array") << "with lines notitle\n";
                        gp.sendBinary1d(pts);
                        gp.flush();
                        count_for_obj += 1;
                    }else
                    {
                        static std::ofstream log_data("./log_currenttime_objectvalue.txt");//
                        log_data<<mpc.curr_time<<" ";
                        log_data<<mpc.object_value_out<<" ";
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);

                }
            } else {
                // Manual driving
                std::string msg = "42[\"manual\",{}]";
                ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
            }
        }
    });

    // We don't need this since we're not using HTTP but if it's removed the
    // program
    // doesn't compile :-(
    h.onHttpRequest([](uWS::HttpResponse *res, uWS::HttpRequest req, char *data,
                       size_t, size_t) {
        const std::string s = "<h1>Hello world!</h1>";
        if (req.getUrl().valueLength == 1) {
            res->end(s.data(), s.length());
        } else {
            // i guess this should be done more gracefully?
            res->end(nullptr, 0);
        }
    });

    h.onConnection([&h](uWS::WebSocket<uWS::SERVER> ws, uWS::HttpRequest req) {
        std::cout << "Connected!!!" << std::endl;
    });

    h.onDisconnection([&h](uWS::WebSocket<uWS::SERVER> ws, int code,
                           char *message, size_t length) {
        ws.close();
        std::cout << "Disconnected" << std::endl;
    });

    int port = 4567;
    if (h.listen(port)) {
        std::cout << "Listening to port " << port << std::endl;
    } else {
        std::cerr << "Failed to listen to port" << std::endl;
        return -1;
    }
    h.run();
}

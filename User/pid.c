// 位置式PID控制算法
#include "pid.h"
PID_struct* PID_init(PID_struct* pid){
    pid->Set=0.0;
    pid->Actual=0.0;
    pid->err=0.0;
    pid->err_last=0.0;
    pid->voltage=0.0;
    pid->integral=0.0;
    pid->Kp=0.2;
    pid->Ki=0;
    pid->Kd=0;
    pid->max_inte = 2000;
    pid->min_inte = -2000;
}
float PID_update(PID_struct* pid,double Set,double Actual){
    Actual = (Actual >1000)?1000:(Actual<-1000)?-1000:Actual;
    pid->Set=Set;
    pid->Actual = Actual;
    pid->err=pid->Set-pid->Actual;
    pid->integral+=pid->err;
    pid->integral = (pid->integral>pid->max_inte)?pid->max_inte:pid->integral;
    pid->integral = (pid->integral<pid->min_inte)?pid->min_inte:pid->integral;
    pid->voltage=pid->Kp*pid->err+pid->Ki*pid->integral * 0.000125 +pid->Kd*(pid->err-pid->err_last) * 8000;
    pid->err_last=pid->err;
    pid->Actual=pid->voltage*1.0;
    return pid->Actual;
}
void PID_set(PID_struct* pid,double p,double i,double d){
    pid->Kp=p;
    pid->Ki=i;
    pid->Kd=d;
}

void PID_setp(PID_struct* pid,double p){
    pid->Kp=p;
}

void PID_seti(PID_struct* pid,double i){
    pid->Ki=i;
}

void PID_setd(PID_struct* pid,double d){
    pid->Kd=d;
}
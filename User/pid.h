// 位置式PID控制算法
typedef struct 
{
    double Set;                //定义设定值
    double Actual;             //定义实际值
    double err;                //定义偏差值
    double err_last;           //定义上一个偏差值
    double Kp;                 //定义比例系数
    double Ki;                 //定义积分系数
    double Kd;                 //定义微分系数
    double voltage;            //定义电压值（控制执行器的变量）
    double integral;           //定义积分值
    double max_inte;
    double min_inte;

}PID_struct;
PID_struct* PID_init(PID_struct*);
float PID_update(PID_struct* pid,double Set,double Actual);
void PID_set(PID_struct* pid,double p,double i,double d);
void PID_setp(PID_struct* pid,double p);

void PID_seti(PID_struct* pid,double i);

void PID_setd(PID_struct* pid,double d);
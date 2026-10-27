#ifndef QMI8658_H
#define QMI8658_H
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/* i2c_master_Init() 이후에 호출. 성공 시 true */
bool qmi8658_init(void);
/* 가속도 mg 단위 (±4g 범위). 실패 시 false */
bool qmi8658_read_mg(float acc[3]);
#ifdef __cplusplus
}
#endif
#endif

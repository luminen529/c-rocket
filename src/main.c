#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TOTAL_TIME 60
#define DT 1.0

#define HORIZONTAL_ACCELERATION 30.0
#define VERTICAL_ACCELERATION 10.0
#define FAILED_HORIZONTAL_ACCELERATION 20.0
#define FAILED_VERTICAL_ACCELERATION -100.0

#define BIAS_SCALE 32.0
#define TARGET_ANGLE 0.0
#define DIAGNOSTIC_ANGLE -70.0
#define ANGLE_LIMIT 45.0
#define SAFE_CONTROL_CHANGE_LIMIT 5.0

typedef enum {
    MODE_UNSAFE,
    MODE_SAFE
} SimulationMode;

typedef enum {
    STATUS_NORMAL,
    STATUS_SAFE_MODE,
    STATUS_FAILED
} SystemStatus;

typedef struct {
    double time;
    double x;
    double altitude;
    double horizontal_velocity;
    double vertical_velocity;
    double angle;
    SystemStatus status;
} RocketState;

typedef struct {
    double raw_bias;
    int16_t converted_bias;
    int conversion_ok;
    double reported_angle;
    int valid;
    int sri_failed;
} SensorData;

/* 모드 이름을 로그와 CSV에서 사용할 문자열로 바꾼다. */
static const char *mode_name(SimulationMode mode)
{
    return mode == MODE_UNSAFE ? "UNSAFE" : "SAFE";
}

/* 시스템 상태를 사람이 읽을 수 있는 문자열로 바꾼다. */
static const char *status_name(SystemStatus status)
{
    if (status == STATUS_SAFE_MODE) {
        return "SAFE_MODE";
    }

    if (status == STATUS_FAILED) {
        return "FAILED";
    }

    return "NORMAL";
}

/* 로켓의 모든 초기 상태를 0과 정상 상태로 설정한다. */
static void init_state(RocketState *rocket)
{
    rocket->time = 0.0;
    rocket->x = 0.0;
    rocket->altitude = 0.0;
    rocket->horizontal_velocity = 0.0;
    rocket->vertical_velocity = 0.0;
    rocket->angle = 0.0;
    rocket->status = STATUS_NORMAL;
}

/* 16비트 정수로 표현할 수 있을 때만 실제 형변환을 수행한다. */
static int convert_bias(double value, int16_t *result)
{
    if (result == NULL) {
        return 0;
    }

    if (value < (double)INT16_MIN || value > (double)INT16_MAX) {
        return 0;
    }

    *result = (int16_t)value;
    return 1;
}

/* 로켓 상태를 읽고 관성 기준 장치의 출력 데이터를 만든다. */
static SensorData run_sri(const RocketState *rocket,
                          SimulationMode mode,
                          int sri_already_failed)
{
    SensorData sensor = {0};

    sensor.raw_bias = rocket->horizontal_velocity * BIAS_SCALE;

    if (sri_already_failed) {
        sensor.sri_failed = 1;
        sensor.reported_angle = mode == MODE_UNSAFE ? DIAGNOSTIC_ANGLE : 0.0;
        sensor.valid = mode == MODE_UNSAFE ? 1 : 0;
        return sensor;
    }

    sensor.conversion_ok = convert_bias(sensor.raw_bias, &sensor.converted_bias);

    if (sensor.conversion_ok) {
        sensor.reported_angle = rocket->angle;
        sensor.valid = 1;
        sensor.sri_failed = 0;
        return sensor;
    }

    sensor.sri_failed = 1;

    if (mode == MODE_UNSAFE) {
        /* 진단 비트가 정상 자세 데이터로 오해된 상황을 단순화한 값이다. */
        sensor.reported_angle = DIAGNOSTIC_ANGLE;
        sensor.valid = 1;
    } else {
        sensor.reported_angle = 0.0;
        sensor.valid = 0;
    }

    return sensor;
}

/* SAFE 모드는 유효하지 않은 새 값 대신 마지막 정상값을 사용한다. */
static double run_navigation(SensorData sensor,
                             double last_valid_angle,
                             SimulationMode mode)
{
    if (mode == MODE_SAFE && sensor.valid == 0) {
        return last_valid_angle;
    }

    return sensor.reported_angle;
}

/* 목표 자세와 측정 자세의 차이로 제어 명령을 계산한다. */
static double run_control(double navigation_angle,
                          double previous_control,
                          SimulationMode mode)
{
    double desired_control = TARGET_ANGLE - navigation_angle;

    if (mode == MODE_SAFE) {
        double change = desired_control - previous_control;

        if (change > SAFE_CONTROL_CHANGE_LIMIT) {
            change = SAFE_CONTROL_CHANGE_LIMIT;
        } else if (change < -SAFE_CONTROL_CHANGE_LIMIT) {
            change = -SAFE_CONTROL_CHANGE_LIMIT;
        }

        return previous_control + change;
    }

    return desired_control;
}

/* 제어 결과를 자세에 적용하고 시스템 상태를 결정한다. */
static void apply_control(RocketState *rocket,
                          SensorData sensor,
                          double control_command,
                          SimulationMode mode)
{
    if (rocket->status == STATUS_FAILED) {
        return;
    }

    rocket->angle += control_command;

    if (rocket->angle > ANGLE_LIMIT ||
        rocket->angle < -ANGLE_LIMIT) {
        rocket->status = STATUS_FAILED;
        return;
    }

    if (mode == MODE_SAFE && sensor.sri_failed) {
        rocket->status = STATUS_SAFE_MODE;
    }
}

/* 다음 1초의 속도와 위치를 단순 산술로 갱신한다. */
static void update_rocket(RocketState *rocket)
{
    if (rocket->status == STATUS_FAILED && rocket->altitude <= 0.0) {
        rocket->horizontal_velocity = 0.0;
        rocket->vertical_velocity = 0.0;
        rocket->time += DT;
        return;
    }

    if (rocket->status == STATUS_FAILED) {
        rocket->horizontal_velocity += FAILED_HORIZONTAL_ACCELERATION * DT;
        rocket->vertical_velocity += FAILED_VERTICAL_ACCELERATION * DT;
    } else {
        rocket->horizontal_velocity += HORIZONTAL_ACCELERATION * DT;
        rocket->vertical_velocity += VERTICAL_ACCELERATION * DT;
    }

    rocket->x += rocket->horizontal_velocity * DT;
    rocket->altitude += rocket->vertical_velocity * DT;

    if (rocket->altitude < 0.0) {
        rocket->altitude = 0.0;
        rocket->horizontal_velocity = 0.0;
        rocket->vertical_velocity = 0.0;
    }

    rocket->time += DT;
}

/* CSV 머리글을 고정된 인터페이스 순서로 기록한다. */
static void write_csv_header(FILE *file)
{
    fprintf(file,
            "mode,time,x,altitude,h_velocity,v_velocity,angle,raw_bias,"
            "converted_bias,reported_angle,sensor_valid,sri_failed,control,status\n");
}

/* 현재 시뮬레이션 상태 한 행을 CSV에 기록한다. */
static void write_csv_row(FILE *file,
                          SimulationMode mode,
                          const RocketState *rocket,
                          SensorData sensor,
                          double control_command)
{
    fprintf(file,
            "%s,%.0f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,",
            mode_name(mode),
            rocket->time,
            rocket->x,
            rocket->altitude,
            rocket->horizontal_velocity,
            rocket->vertical_velocity,
            rocket->angle,
            sensor.raw_bias);

    if (sensor.conversion_ok) {
        fprintf(file, "%d", (int)sensor.converted_bias);
    }

    fprintf(file,
            ",%.2f,%d,%d,%.2f,%s\n",
            sensor.reported_angle,
            sensor.valid,
            sensor.sri_failed,
            control_command,
            status_name(rocket->status));
}

/* 매초 핵심 상태를 한 줄로 출력한다. */
static void print_step_log(SimulationMode mode,
                           const RocketState *rocket,
                           SensorData sensor,
                           double control_command)
{
    printf("[%s] T+%02.0f | alt=%8.2f | bias=%8.0f | valid=%d | "
           "control=%6.1f | %s\n",
           mode_name(mode),
           rocket->time,
           rocket->altitude,
           sensor.raw_bias,
           sensor.valid,
           control_command,
           status_name(rocket->status));
}

/* 최초 오류 순간에 모듈 사이의 실패 전파 순서를 보여준다. */
static void print_failure_chain(SimulationMode mode,
                                const RocketState *rocket,
                                double navigation_angle,
                                double control_command)
{
    printf("  -> RANGE ERROR: %.0f is outside int16_t\n",
           rocket->horizontal_velocity * BIAS_SCALE);

    if (mode == MODE_UNSAFE) {
        printf("  -> SRI FAILED: diagnostic data marked as valid\n");
        printf("  -> NAVIGATION: diagnostic angle %.1f accepted\n",
               navigation_angle);
        printf("  -> CONTROL: abnormal command %.1f applied\n",
               control_command);
    } else {
        printf("  -> SRI INVALID: unsafe conversion blocked\n");
        printf("  -> NAVIGATION: last valid angle %.1f kept\n",
               navigation_angle);
        printf("  -> CONTROL: safe command %.1f applied\n",
               control_command);
    }
}

/* 한 모드의 0초부터 60초까지를 실행하고 CSV를 생성한다. */
static int run_simulation(SimulationMode mode, const char *file_name)
{
    FILE *file = fopen(file_name, "w");
    RocketState rocket;
    double last_valid_angle = 0.0;
    double previous_control = 0.0;
    int failure_reported = 0;
    int sri_failed_latched = 0;
    int step;

    if (file == NULL) {
        fprintf(stderr, "Cannot open output file: %s\n", file_name);
        return 0;
    }

    init_state(&rocket);
    write_csv_header(file);
    printf("\n=== %s MODE ===\n", mode_name(mode));

    for (step = 0; step <= TOTAL_TIME; ++step) {
        SensorData sensor = run_sri(&rocket, mode, sri_failed_latched);
        double navigation_angle;
        double control_command;

        if (sensor.sri_failed) {
            sri_failed_latched = 1;
        }

        if (sensor.conversion_ok) {
            last_valid_angle = sensor.reported_angle;
        }

        navigation_angle = run_navigation(sensor, last_valid_angle, mode);
        control_command = run_control(navigation_angle, previous_control, mode);
        apply_control(&rocket, sensor, control_command, mode);

        print_step_log(mode, &rocket, sensor, control_command);

        if (sensor.sri_failed && failure_reported == 0) {
            print_failure_chain(mode, &rocket, navigation_angle, control_command);
            failure_reported = 1;
        }

        write_csv_row(file, mode, &rocket, sensor, control_command);
        previous_control = control_command;

        if (step < TOTAL_TIME) {
            update_rocket(&rocket);
        }
    }

    fclose(file);
    return 1;
}

/* 형변환 경계값과 실패 시 출력값 보존을 확인한다. */
static int run_conversion_tests(void)
{
    int passed = 0;
    int16_t result;

    result = 0;
    if (convert_bias(32767.0, &result) && result == INT16_MAX) {
        ++passed;
    }

    result = 1234;
    if (!convert_bias(32768.0, &result) && result == 1234) {
        ++passed;
    }

    result = 0;
    if (convert_bias(-32768.0, &result) && result == INT16_MIN) {
        ++passed;
    }

    result = 1234;
    if (!convert_bias(-32769.0, &result) && result == 1234) {
        ++passed;
    }

    result = 1234;
    if (!convert_bias(0.0, NULL) && result == 1234) {
        ++passed;
    }

    printf("Conversion tests: %d/5 passed\n", passed);
    return passed == 5;
}

int main(int argc, char *argv[])
{
    if (argc == 2 && strcmp(argv[1], "--test") == 0) {
        return run_conversion_tests() ? 0 : 1;
    }

    if (argc != 1) {
        fprintf(stderr, "Usage: %s [--test]\n", argv[0]);
        return 1;
    }

    if (!run_simulation(MODE_UNSAFE, "output/unsafe.csv")) {
        return 1;
    }

    if (!run_simulation(MODE_SAFE, "output/safe.csv")) {
        return 1;
    }

    printf("\nCSV files created in output/.\n");
    return 0;
}

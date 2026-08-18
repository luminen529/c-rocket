#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#ifndef C_ROCKET_OUTPUT_DIR
#define C_ROCKET_OUTPUT_DIR "output"
#endif

#define TOTAL_TIME 60
#define DT 1.0  // 갱신 간격
// 시뮬레이션 상수 -> 단순화됨
#define HORIZONTAL_ACCELERATION 30.0
#define VERTICAL_ACCELERATION 5.0
#define BIAS_SCALE 30.0  // 수평 속도 센서 변환 상수
#define ABNORMAL_TURN_RATE 12.0  // 비정상적인 회전 속도
#define ANGLE_OF_ATTACK_LIMIT 20.0  // 한계 각도
/* SRI는 프랑스어 Système de Référence Inertielle의 약자로, 영어로는 Inertial Reference System,
 * 한국어로는 관성 기준 장치입니다. -> 실패 이벤트 발생 관련 정의인듯 */
#define SRI_CYCLE_SECONDS 0.072
#define ACTIVE_SRI_FAILURE_TIME 37.0
#define BACKUP_SRI_FAILURE_TIME \
    (ACTIVE_SRI_FAILURE_TIME - SRI_CYCLE_SECONDS)
#define BREAKUP_TIME 39.0

typedef enum {
    MODE_UNSAFE,  // 같은 정렬 변환을 보호 없이 실행하는 사고 경로
    MODE_SAFE  // 같은 정렬 변환을 실행하되 범위를 벗어난 변환을 차단하는 보호 경로
} SimulationMode;

typedef enum {
    STATUS_NORMAL,
    STATUS_CONTROL_LOST,
    STATUS_FAILED
} SystemStatus;

typedef enum {
    CONVERSION_NOT_RUN,
    CONVERSION_OK,
    CONVERSION_OPERAND_ERROR,
    CONVERSION_BLOCKED
} ConversionResult;

typedef enum {
    DATA_FLIGHT,
    DATA_DIAGNOSTIC,
    DATA_INVALID
} DataKind;

typedef enum {
    NOZZLE_NEUTRAL,
    NOZZLE_FULL_DEFLECTION
} NozzleCommand;

typedef struct {
    int operational;
    double failure_time;
} SRIState;

typedef struct {
    double time;
    double x;
    double altitude;
    double horizontal_velocity;
    double vertical_velocity;
    double angle;
    SystemStatus status;
} RocketState;  // 로케트 상태 구조체

typedef struct {
    int bias_calculated;
    double raw_bias;
    int16_t converted_bias;
    ConversionResult conversion_result;
    DataKind output_kind;
    int valid;
} SensorData;

typedef struct {
    SRIState sri1_backup;
    SRIState sri2_active;
    DataKind obc_input;
    NozzleCommand nozzle_command;
} GuidanceSystem;

typedef SensorData (*SriRunner)(GuidanceSystem *system, const RocketState *rocket);

static const char *mode_name(SimulationMode mode)
{
    return mode == MODE_UNSAFE ? "UNSAFE" : "SAFE";
}

static const char *status_name(SystemStatus status)
{
    if (status == STATUS_CONTROL_LOST) {
        return "CONTROL_LOST";
    }

    if (status == STATUS_FAILED) {
        return "FAILED";
    }

    return "NORMAL";
}

static const char *conversion_name(ConversionResult result)
{
    if (result == CONVERSION_OK) {
        return "OK";
    }

    if (result == CONVERSION_OPERAND_ERROR) {
        return "OPERAND_ERROR";
    }

    if (result == CONVERSION_BLOCKED) {
        return "BLOCKED";
    }

    return "NOT_RUN";
}

static const char *data_kind_name(DataKind kind)
{
    if (kind == DATA_DIAGNOSTIC) {
        return "DIAGNOSTIC";
    }

    if (kind == DATA_INVALID) {
        return "INVALID";
    }

    return "FLIGHT";
}

static const char *nozzle_name(NozzleCommand command)
{
    return command == NOZZLE_FULL_DEFLECTION
               ? "FULL_DEFLECTION"
               : "NEUTRAL";
}

static const char *sri_status_name(const SRIState *sri)
{
    return sri->operational ? "RUNNING" : "STOPPED";
}

// rocket 상태 init
static void init_rocket(RocketState *rocket)
{
    rocket->time = 0.0;
    rocket->x = 0.0;
    rocket->altitude = 0.0;
    rocket->horizontal_velocity = 0.0;
    rocket->vertical_velocity = 0.0;
    rocket->angle = 0.0;
    rocket->status = STATUS_NORMAL;
}

static void init_sri(SRIState *sri)
{
    sri->operational = 1;
    sri->failure_time = -1.0;
}

static void init_guidance(GuidanceSystem *system)
{
    init_sri(&system->sri1_backup);
    init_sri(&system->sri2_active);
    system->obc_input = DATA_FLIGHT;
    system->nozzle_command = NOZZLE_NEUTRAL;
}

// 변환 범위 검사
static int is_in_int16_range(double value)
{
    return value >= (double)INT16_MIN && value <= (double)INT16_MAX;
}

// unsafe 변환
static ConversionResult convert_bias_unsafe(double value, int16_t *result)
{
    if (!is_in_int16_range(value)) {
        return CONVERSION_OPERAND_ERROR;
    }

    if (result == NULL) {
        return CONVERSION_OPERAND_ERROR;
    }

    *result = (int16_t)value;
    return CONVERSION_OK;
}

// safe 변환
static ConversionResult convert_bias_safe(double value, int16_t *result)
{
    if (result == NULL || !is_in_int16_range(value)) {
        return CONVERSION_BLOCKED;
    }

    *result = (int16_t)value;
    return CONVERSION_OK;
}

static void stop_sri(SRIState *sri, double failure_time)
{
    sri->operational = 0;
    sri->failure_time = failure_time;
}

static SensorData initial_sensor_data(void)
{
    SensorData sensor = {0};
    sensor.conversion_result = CONVERSION_NOT_RUN;
    sensor.output_kind = DATA_FLIGHT;
    sensor.valid = 1;

    return sensor;
}

/* 실제 사고 경로: 범위를 넘으면 두 SRI가 정지하고 진단 패턴을 내보낸다. */
static SensorData run_sri_unsafe(GuidanceSystem *system,
                                 const RocketState *rocket)
{
    SensorData sensor = initial_sensor_data();

    if (!system->sri2_active.operational) {
        /* 정지한 SRI는 비행 데이터 대신 진단 패턴을 내보낸다. */
        sensor.output_kind = DATA_DIAGNOSTIC;
        sensor.valid = 0;
        return sensor;
    }

    sensor.bias_calculated = 1;
    sensor.raw_bias = rocket->horizontal_velocity * BIAS_SCALE;
    sensor.conversion_result =
        convert_bias_unsafe(sensor.raw_bias, &sensor.converted_bias);

    if (sensor.conversion_result == CONVERSION_OPERAND_ERROR) {
        /*
         * UNSAFE 사고 경로: 활성 SRI의 변환 오류와 그보다 72 ms 앞선
         * 백업 SRI의 정지를 함께 기록한다. 따라서 백업 전환도 불가능하다.
         */
        stop_sri(&system->sri1_backup, BACKUP_SRI_FAILURE_TIME);
        stop_sri(&system->sri2_active, ACTIVE_SRI_FAILURE_TIME);
        sensor.output_kind = DATA_DIAGNOSTIC;
        sensor.valid = 0;
    }

    return sensor;
}

/* 보호 경로: 같은 변환을 실행하되 범위를 넘으면 SRI를 살리고 입력을 무효화한다. */
static SensorData run_sri_safe(GuidanceSystem *system,
                               const RocketState *rocket)
{
    SensorData sensor = initial_sensor_data();

    if (!system->sri2_active.operational) {
        sensor.output_kind = DATA_INVALID;
        sensor.valid = 0;
        return sensor;
    }

    sensor.bias_calculated = 1;
    sensor.raw_bias = rocket->horizontal_velocity * BIAS_SCALE;
    sensor.conversion_result =
        convert_bias_safe(sensor.raw_bias, &sensor.converted_bias);

    if (sensor.conversion_result != CONVERSION_OK) {
        sensor.output_kind = DATA_INVALID;
        sensor.valid = 0;
    }

    return sensor;
}

static void run_obc(GuidanceSystem *system, const SensorData *sensor)
{
    if (sensor->valid && sensor->output_kind == DATA_FLIGHT) {
        /* 두 모드의 정상 경로: 비행 데이터를 사용하고 노즐은 중립을 유지한다. */
        system->obc_input = DATA_FLIGHT;
        system->nozzle_command = NOZZLE_NEUTRAL;
        return;
    }

    if (sensor->output_kind == DATA_DIAGNOSTIC) {
        /*
         * 사고 경로: 진단 패턴을 비행 데이터처럼 잘못 받아들인다.
         * 그 결과 OBC가 노즐을 끝까지 편향시키고 제어 상실을 유발한다.
         */
        system->obc_input = DATA_DIAGNOSTIC;
        system->nozzle_command = NOZZLE_FULL_DEFLECTION;
        return;
    }

    /* SAFE 경로: 무효하거나 진단용인 입력은 사용하지 않고 노즐을 중립에 둔다. */
    system->obc_input = DATA_INVALID;
    system->nozzle_command = NOZZLE_NEUTRAL;
}

static void apply_guidance(RocketState *rocket,
                           const GuidanceSystem *system)
{
    if (rocket->status == STATUS_FAILED) {
        return;
    }

    /* FULL_DEFLECTION 명령은 위의 UNSAFE OBC 분기에서만 만들어진다. */
    if (system->nozzle_command == NOZZLE_FULL_DEFLECTION &&
        rocket->status == STATUS_NORMAL) {
        rocket->status = STATUS_CONTROL_LOST;
    }

    /* 제어 상실 뒤 자세가 계속 틀어져 받음각 한계를 넘으면 기체가 파괴된다. */
    if (rocket->status == STATUS_CONTROL_LOST &&
        rocket->time >= BREAKUP_TIME &&
        fabs(rocket->angle) > ANGLE_OF_ATTACK_LIMIT) {
        rocket->status = STATUS_FAILED;
    }
}

static void update_rocket(RocketState *rocket)
{
    if (rocket->status == STATUS_FAILED) {
        /* The real launcher broke up and self-destructed; do not model a fall. */
        rocket->time += DT;
        return;
    }

    rocket->horizontal_velocity += HORIZONTAL_ACCELERATION * DT;
    rocket->vertical_velocity += VERTICAL_ACCELERATION * DT;
    rocket->x += rocket->horizontal_velocity * DT;
    rocket->altitude += rocket->vertical_velocity * DT;

    if (rocket->status == STATUS_CONTROL_LOST) {
        rocket->angle += ABNORMAL_TURN_RATE * DT;
    }

    rocket->time += DT;
}

static void write_csv_header(FILE *file)
{
    fprintf(file,
            "mode,time,x,altitude,h_velocity,v_velocity,angle,raw_bias,"
            "converted_bias,conversion_result,sri1_status,sri2_status,"
            "sri1_failure_time,sri2_failure_time,obc_input,nozzle_command,status\n");
}

static void write_optional_time(FILE *file, const SRIState *sri)
{
    if (sri->failure_time >= 0.0) {
        fprintf(file, "%.3f", sri->failure_time);
    }
}

static void write_csv_row(FILE *file,
                          SimulationMode mode,
                          const RocketState *rocket,
                          const GuidanceSystem *system,
                          const SensorData *sensor)
{
    fprintf(file,
            "%s,%.0f,%.2f,%.2f,%.2f,%.2f,%.2f,",
            mode_name(mode),
            rocket->time,
            rocket->x,
            rocket->altitude,
            rocket->horizontal_velocity,
            rocket->vertical_velocity,
            rocket->angle);

    if (sensor->bias_calculated) {
        fprintf(file, "%.2f", sensor->raw_bias);
    }

    fputc(',', file);
    if (sensor->conversion_result == CONVERSION_OK) {
        fprintf(file, "%d", (int)sensor->converted_bias);
    }

    fprintf(file,
            ",%s,%s,%s,",
            conversion_name(sensor->conversion_result),
            sri_status_name(&system->sri1_backup),
            sri_status_name(&system->sri2_active));

    write_optional_time(file, &system->sri1_backup);
    fputc(',', file);
    write_optional_time(file, &system->sri2_active);

    fprintf(file,
            ",%s,%s,%s\n",
            data_kind_name(system->obc_input),
            nozzle_name(system->nozzle_command),
            status_name(rocket->status));
}

static void print_console_log(SimulationMode mode,
                           const RocketState *rocket,
                           const GuidanceSystem *system,
                           const SensorData *sensor)
{
    printf("[%s] T+%02.0f | alt=%8.2f | SRI1=%-7s | SRI2=%-7s | "
           "OBC=%-10s | nozzle=%-15s | %s\n",
           mode_name(mode),
           rocket->time,
           rocket->altitude,
           sri_status_name(&system->sri1_backup),
           sri_status_name(&system->sri2_active),
           data_kind_name(system->obc_input),
           nozzle_name(system->nozzle_command),
           status_name(rocket->status));

    if (sensor->conversion_result == CONVERSION_OPERAND_ERROR) {
        printf("  -> %.3f: backup SRI 1 stopped (previous 72 ms cycle)\n",
               system->sri1_backup.failure_time);
        printf("  -> %.3f: active SRI 2 Operand Error, BH=%.0f\n",
               system->sri2_active.failure_time,
               sensor->raw_bias);
        printf("  -> OBC: backup unavailable; diagnostic pattern accepted\n");
        printf("  -> CONTROL: full nozzle deflection commanded\n");
    }

    if (rocket->status == STATUS_FAILED && rocket->time == BREAKUP_TIME) {
        printf("  -> BREAKUP: angle of attack exceeded %.0f degrees\n",
               ANGLE_OF_ATTACK_LIMIT);
        printf("  -> NEUTRALISATION: self-destruct triggered\n");
    }
}

static int create_output_directory(void)
{
    int result;

#if defined(_WIN32)
    result = _mkdir(C_ROCKET_OUTPUT_DIR);
#else
    result = mkdir(C_ROCKET_OUTPUT_DIR, 0777);
#endif

    if (result == 0 || errno == EEXIST) {
        return 0;
    }
    fprintf(stderr, "Cannot create output directory: %s\n", C_ROCKET_OUTPUT_DIR);
    return 1;
}

static int run_simulation(SimulationMode mode, const char *file_name)
{
    // 선언과 init
    /*
     * 파일은 FILE 구조체의 특성상 포인터 변수로 참조한다. *file로 "file"이라는 포인터 변수 선언
     * fopen() 함수는 파일을 열고 FILE 구조체를 반환하며, 이 구조체는 파일에 대한 정보를 담고 있다. 'w'는 쓰기 모드
     * fopen(filename, mode)
     */
    FILE *file = fopen(file_name, "w");
    RocketState rocket;
    GuidanceSystem system;
    // 반복 횟수 = 시뮬레이션 시간
    // file을 불러올 수 없을 떄 예외처리
    if (file == NULL) {
        fprintf(stderr, "Cannot open output file: %s\n", file_name);
        return 1;
    }
    init_rocket(&rocket);
    init_guidance(&system);
    SriRunner sri_runner =
        mode == MODE_UNSAFE ? &run_sri_unsafe : &run_sri_safe;
    write_csv_header(file);
    printf("\n=== SITUATION: %s ===\n", mode_name(mode));

    for (int step = 0; step <= TOTAL_TIME; ++step) {
        SensorData sensor = sri_runner(&system, &rocket);

        run_obc(&system, &sensor);
        apply_guidance(&rocket, &system);
        print_console_log(mode, &rocket, &system, &sensor);
        write_csv_row(file, mode, &rocket, &system, &sensor);

        if (step < TOTAL_TIME) {
            update_rocket(&rocket);
        }
    }
    fclose(file);
    return 0;
}

int main(void)
{
    if (create_output_directory()) {
        return 1;
    }
    if (run_simulation(MODE_UNSAFE, C_ROCKET_OUTPUT_DIR "/unsafe.csv")) {
        return 1;
    }
    if (run_simulation(MODE_SAFE, C_ROCKET_OUTPUT_DIR "/safe.csv")) {
        return 1;
    }
    printf("\nDONE.\n");
    return 0;
}

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TOTAL_TIME 60
#define DT 1.0

/* The flight dynamics are intentionally simple educational values. */
#define HORIZONTAL_ACCELERATION 30.0
#define VERTICAL_ACCELERATION 5.0
#define BIAS_SCALE 30.0
#define ABNORMAL_TURN_RATE 12.0
#define ANGLE_OF_ATTACK_LIMIT 20.0

/* The active SRI fails one 72 ms data cycle after the backup SRI. */
#define SRI_CYCLE_SECONDS 0.072
#define ACTIVE_SRI_FAILURE_TIME 37.0
#define BACKUP_SRI_FAILURE_TIME \
    (ACTIVE_SRI_FAILURE_TIME - SRI_CYCLE_SECONDS)
#define BREAKUP_TIME 39.0

typedef enum {
    MODE_UNSAFE,
    MODE_SAFE
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
    int alignment_active;
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
} RocketState;

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

static void init_sri(SRIState *sri, SimulationMode mode)
{
    sri->operational = 1;
    /* Ariane 5 did not need the inherited alignment task after lift-off. */
    sri->alignment_active = mode == MODE_UNSAFE ? 1 : 0;
    sri->failure_time = -1.0;
}

static void init_guidance(GuidanceSystem *system, SimulationMode mode)
{
    init_sri(&system->sri1_backup, mode);
    init_sri(&system->sri2_active, mode);
    system->obc_input = DATA_FLIGHT;
    system->nozzle_command = NOZZLE_NEUTRAL;
}

/*
 * Model the Ada conversion without executing an out-of-range C cast.
 * Such a cast is undefined behavior in C and would not deterministically
 * reproduce the SRI processor's Operand Error.
 */
static ConversionResult model_bias_conversion(double value,
                                               SimulationMode mode,
                                               int alignment_active,
                                               int16_t *result)
{
    if (!alignment_active) {
        return CONVERSION_NOT_RUN;
    }

    if (value < (double)INT16_MIN || value > (double)INT16_MAX) {
        return mode == MODE_UNSAFE
                   ? CONVERSION_OPERAND_ERROR
                   : CONVERSION_BLOCKED;
    }

    if (result == NULL) {
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

/* Run the active SRI and model the preceding backup-SRI data cycle. */
static SensorData run_sri(GuidanceSystem *system,
                          const RocketState *rocket,
                          SimulationMode mode)
{
    SensorData sensor = {0};

    sensor.conversion_result = CONVERSION_NOT_RUN;
    sensor.output_kind = DATA_FLIGHT;
    sensor.valid = 1;

    if (!system->sri2_active.operational) {
        sensor.output_kind = DATA_DIAGNOSTIC;
        sensor.valid = 0;
        return sensor;
    }

    if (!system->sri2_active.alignment_active) {
        return sensor;
    }

    sensor.bias_calculated = 1;
    sensor.raw_bias = rocket->horizontal_velocity * BIAS_SCALE;
    sensor.conversion_result = model_bias_conversion(
        sensor.raw_bias,
        mode,
        system->sri2_active.alignment_active,
        &sensor.converted_bias
    );

    if (sensor.conversion_result == CONVERSION_OK) {
        return sensor;
    }

    if (sensor.conversion_result == CONVERSION_OPERAND_ERROR) {
        stop_sri(&system->sri1_backup, BACKUP_SRI_FAILURE_TIME);
        stop_sri(&system->sri2_active, ACTIVE_SRI_FAILURE_TIME);
        sensor.output_kind = DATA_DIAGNOSTIC;
        sensor.valid = 0;
    } else {
        sensor.output_kind = DATA_INVALID;
        sensor.valid = 0;
    }

    return sensor;
}

static void run_obc(GuidanceSystem *system,
                    const SensorData *sensor,
                    SimulationMode mode)
{
    if (sensor->valid && sensor->output_kind == DATA_FLIGHT) {
        system->obc_input = DATA_FLIGHT;
        system->nozzle_command = NOZZLE_NEUTRAL;
        return;
    }

    if (mode == MODE_UNSAFE && sensor->output_kind == DATA_DIAGNOSTIC) {
        /* Flight 501 interpreted the active SRI diagnostic pattern as flight data. */
        system->obc_input = DATA_DIAGNOSTIC;
        system->nozzle_command = NOZZLE_FULL_DEFLECTION;
        return;
    }

    system->obc_input = DATA_INVALID;
    system->nozzle_command = NOZZLE_NEUTRAL;
}

static void apply_guidance(RocketState *rocket,
                           const GuidanceSystem *system)
{
    if (rocket->status == STATUS_FAILED) {
        return;
    }

    if (system->nozzle_command == NOZZLE_FULL_DEFLECTION &&
        rocket->status == STATUS_NORMAL) {
        rocket->status = STATUS_CONTROL_LOST;
    }

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

static void print_step_log(SimulationMode mode,
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

static int run_simulation(SimulationMode mode, const char *file_name)
{
    FILE *file = fopen(file_name, "w");
    RocketState rocket;
    GuidanceSystem system;
    int step;

    if (file == NULL) {
        fprintf(stderr, "Cannot open output file: %s\n", file_name);
        return 0;
    }

    init_rocket(&rocket);
    init_guidance(&system, mode);
    write_csv_header(file);
    printf("\n=== %s MODE ===\n", mode_name(mode));

    for (step = 0; step <= TOTAL_TIME; ++step) {
        SensorData sensor = run_sri(&system, &rocket, mode);

        run_obc(&system, &sensor, mode);
        apply_guidance(&rocket, &system);
        print_step_log(mode, &rocket, &system, &sensor);
        write_csv_row(file, mode, &rocket, &system, &sensor);

        if (step < TOTAL_TIME) {
            update_rocket(&rocket);
        }
    }

    fclose(file);
    return 1;
}

static int run_conversion_tests(void)
{
    int passed = 0;
    int16_t result;

    result = 0;
    if (model_bias_conversion(32767.0, MODE_UNSAFE, 1, &result) ==
            CONVERSION_OK &&
        result == INT16_MAX) {
        ++passed;
    }

    result = 1234;
    if (model_bias_conversion(32768.0, MODE_UNSAFE, 1, &result) ==
            CONVERSION_OPERAND_ERROR &&
        result == 1234) {
        ++passed;
    }

    result = 0;
    if (model_bias_conversion(-32768.0, MODE_UNSAFE, 1, &result) ==
            CONVERSION_OK &&
        result == INT16_MIN) {
        ++passed;
    }

    result = 1234;
    if (model_bias_conversion(-32769.0, MODE_UNSAFE, 1, &result) ==
            CONVERSION_OPERAND_ERROR &&
        result == 1234) {
        ++passed;
    }

    result = 1234;
    if (model_bias_conversion(32768.0, MODE_SAFE, 1, &result) ==
            CONVERSION_BLOCKED &&
        result == 1234) {
        ++passed;
    }

    result = 1234;
    if (model_bias_conversion(0.0, MODE_SAFE, 0, &result) ==
            CONVERSION_NOT_RUN &&
        result == 1234) {
        ++passed;
    }

    result = 1234;
    if (model_bias_conversion(0.0, MODE_SAFE, 1, NULL) ==
            CONVERSION_BLOCKED &&
        result == 1234) {
        ++passed;
    }

    printf("Conversion tests: %d/7 passed\n", passed);
    return passed == 7;
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

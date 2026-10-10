// Host-side regression tests for the merged Bambu MQTT -> SystemState logic.
// Run with: pio test -e native
#include <ArduinoJson.h>
#include <unity.h>

#include "PrinterState.h"

namespace {
ArduinoJson::JsonVariantConst parsePrint(ArduinoJson::JsonDocument& doc, const char* json) {
  ArduinoJson::deserializeJson(doc, json);
  return doc.as<ArduinoJson::JsonVariantConst>();
}
}  // namespace

void test_idle_when_no_stage_progress_or_state() {
  PrinterStateMachine sm;
  ArduinoJson::JsonDocument doc;
  auto print = parsePrint(doc, R"({"nozzle_temper": 25.0})");
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SystemState::Idle), static_cast<int>(sm.update(print, 1000)));
}

void test_printing_from_gcode_state_running() {
  PrinterStateMachine sm;
  ArduinoJson::JsonDocument doc;
  auto print = parsePrint(doc, R"({"gcode_state":"RUNNING","mc_percent":45})");
  SystemState s = sm.update(print, 1000);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SystemState::Printing), static_cast<int>(s));
  TEST_ASSERT_EQUAL_UINT8(45, sm.telemetry().progressPercent);
}

void test_stg_cur_255_without_prior_job_is_idle() {
  PrinterStateMachine sm;
  ArduinoJson::JsonDocument doc;
  auto print = parsePrint(doc, R"({"stg_cur":255})");
  SystemState s = sm.update(print, 1000);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SystemState::Idle), static_cast<int>(s));
}

void test_sticky_finish_on_connect_is_idle() {
  PrinterStateMachine sm;
  ArduinoJson::JsonDocument doc;
  auto print = parsePrint(doc, R"({"gcode_state":"FINISH","stg_cur":255,"mc_percent":100})");
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SystemState::Idle), static_cast<int>(sm.update(print, 1000)));
}

void test_finished_after_observed_running() {
  PrinterStateMachine sm;
  ArduinoJson::JsonDocument run;
  sm.update(parsePrint(run, R"({"gcode_state":"RUNNING","mc_percent":99})"), 1000);
  ArduinoJson::JsonDocument done;
  SystemState s = sm.update(parsePrint(done, R"({"gcode_state":"FINISH","stg_cur":255})"), 2000);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SystemState::Finished), static_cast<int>(s));
  TEST_ASSERT_EQUAL_UINT8(100, sm.telemetry().progressPercent);
}

void test_paused_from_gcode_state_pause() {
  PrinterStateMachine sm;
  ArduinoJson::JsonDocument doc;
  auto print = parsePrint(doc, R"({"gcode_state":"PAUSE"})");
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SystemState::Paused), static_cast<int>(sm.update(print, 1000)));
}

// PrintSphere: HMS alone warns during a job; it is not a hard failed/ready break.
void test_hms_alone_does_not_force_error() {
  PrinterStateMachine sm;
  ArduinoJson::JsonDocument doc;
  auto print = parsePrint(doc, R"({"gcode_state":"IDLE","hms":[{"attr":123,"code":456}]})");
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SystemState::Idle), static_cast<int>(sm.update(print, 1000)));
}

void test_print_error_triggers_error() {
  PrinterStateMachine sm;
  ArduinoJson::JsonDocument doc;
  auto print = parsePrint(doc, R"({"gcode_state":"PAUSE","print_error":12345})");
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SystemState::Error), static_cast<int>(sm.update(print, 1000)));
}

// User cancel: FAILED with no print_error / HMS → PrintSphere "ready".
void test_failed_without_concrete_error_is_idle() {
  PrinterStateMachine sm;
  ArduinoJson::JsonDocument run;
  sm.update(parsePrint(run, R"({"gcode_state":"RUNNING","mc_percent":10})"), 1000);
  ArduinoJson::JsonDocument abortDoc;
  SystemState s =
      sm.update(parsePrint(abortDoc, R"({"gcode_state":"FAILED","print_error":0,"hms":[],"stg_cur":255})"), 2000);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SystemState::Idle), static_cast<int>(s));
}

void test_failed_with_print_error_is_error() {
  PrinterStateMachine sm;
  ArduinoJson::JsonDocument doc;
  auto print = parsePrint(doc, R"({"gcode_state":"FAILED","print_error":50364435,"hms":[]})");
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SystemState::Error), static_cast<int>(sm.update(print, 1000)));
}

void test_heating_progress_scales_to_actual_target_not_fixed_200() {
  PrinterStateMachine sm;

  ArduinoJson::JsonDocument baselineDoc;
  sm.update(parsePrint(baselineDoc, R"({"nozzle_temper":25.0,"nozzle_target_temper":280.0})"), 1000);

  ArduinoJson::JsonDocument heatingDoc;
  SystemState s = sm.update(parsePrint(heatingDoc, R"({"nozzle_temper":250.0,"nozzle_target_temper":280.0})"), 2000);

  TEST_ASSERT_EQUAL_INT(static_cast<int>(SystemState::Heating), static_cast<int>(s));
  TEST_ASSERT_TRUE(sm.telemetry().progressPercent > 80);
  TEST_ASSERT_TRUE(sm.telemetry().progressPercent < 100);
}

void test_heating_completes_when_target_reached() {
  PrinterStateMachine sm;
  ArduinoJson::JsonDocument baselineDoc;
  sm.update(parsePrint(baselineDoc, R"({"nozzle_temper":25.0,"nozzle_target_temper":220.0})"), 1000);

  ArduinoJson::JsonDocument heatingDoc;
  sm.update(parsePrint(heatingDoc, R"({"nozzle_temper":100.0,"nozzle_target_temper":220.0})"), 2000);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SystemState::Heating), static_cast<int>(sm.state()));

  ArduinoJson::JsonDocument doneDoc;
  SystemState s = sm.update(parsePrint(doneDoc, R"({"nozzle_temper":219.0,"nozzle_target_temper":220.0})"), 3000);
  // Without a print job, preheat complete → Preparing (PrintSphere handoff).
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SystemState::Preparing), static_cast<int>(s));
}

void test_printing_works_without_stg_cur_field() {
  PrinterStateMachine sm;
  ArduinoJson::JsonDocument doc;
  auto print = parsePrint(doc, R"({"gcode_state":"RUNNING","mc_percent":72,"nozzle_temper":210.0})");
  SystemState s = sm.update(print, 1000);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SystemState::Printing), static_cast<int>(s));
  TEST_ASSERT_EQUAL_UINT8(72, sm.telemetry().progressPercent);
}

void test_downloading_from_prepare_percent() {
  PrinterStateMachine sm;
  ArduinoJson::JsonDocument doc;
  auto print = parsePrint(doc, R"({"gcode_file_prepare_percent":40,"nozzle_temper":25.0})");
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SystemState::Downloading), static_cast<int>(sm.update(print, 1000)));
}

// PrintSphere: cleaning_nozzle_tip (14) is "clean nozzle" / preparing, not bed level.
void test_clean_nozzle_stg_cur_14_is_preparing() {
  PrinterStateMachine sm;
  ArduinoJson::JsonDocument doc;
  auto print = parsePrint(doc, R"({"gcode_state":"PREPARE","stg_cur":14})");
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SystemState::Preparing), static_cast<int>(sm.update(print, 1000)));
}

void test_homing_from_stg_cur_13_during_prepare() {
  PrinterStateMachine sm;
  ArduinoJson::JsonDocument doc;
  auto print = parsePrint(doc, R"({"gcode_state":"PREPARE","stg_cur":13})");
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SystemState::Homing), static_cast<int>(sm.update(print, 1000)));
}

// PrintSphere resolved_stage_from_payload: terminal IDLE ignores stg_cur activity.
void test_idle_gcode_ignores_homing_stage() {
  PrinterStateMachine sm;
  ArduinoJson::JsonDocument doc;
  auto print = parsePrint(doc, R"({"gcode_state":"IDLE","stg_cur":13,"nozzle_temper":25.0})");
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SystemState::Idle), static_cast<int>(sm.update(print, 1000)));
}

void test_idle_with_stg_cur_0_stays_idle() {
  PrinterStateMachine sm;
  ArduinoJson::JsonDocument doc;
  auto print = parsePrint(doc, R"({"gcode_state":"IDLE","stg_cur":0,"nozzle_temper":25.0})");
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SystemState::Idle), static_cast<int>(sm.update(print, 1000)));
}

void test_printsphere_heating_hotend_stage() {
  PrinterStateMachine sm;
  ArduinoJson::JsonDocument doc;
  auto print = parsePrint(doc, R"({"gcode_state":"PREPARE","stg_cur":7})");
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SystemState::Heating), static_cast<int>(sm.update(print, 1000)));
}

void test_printsphere_filament_change_during_pause() {
  PrinterStateMachine sm;
  ArduinoJson::JsonDocument doc;
  auto print = parsePrint(doc, R"({"gcode_state":"PAUSE","stg_cur":4})");
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SystemState::Preparing), static_cast<int>(sm.update(print, 1000)));
}

void test_home_flag_does_not_force_homing() {
  PrinterStateMachine sm;
  ArduinoJson::JsonDocument idleDoc;
  sm.update(parsePrint(idleDoc, R"({"gcode_state":"IDLE","home_flag":31,"nozzle_temper":25.0})"), 1000);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SystemState::Idle), static_cast<int>(sm.state()));

  ArduinoJson::JsonDocument runDoc;
  SystemState s =
      sm.update(parsePrint(runDoc, R"({"gcode_state":"RUNNING","mc_percent":20,"home_flag":31})"), 2000);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SystemState::Printing), static_cast<int>(s));
}

// Partial MQTT: stage latches; later progress-only packet under RUNNING clears prep.
void test_stale_prep_cleared_when_running_progress_advances() {
  PrinterStateMachine sm;
  ArduinoJson::JsonDocument prep;
  sm.update(parsePrint(prep, R"({"gcode_state":"RUNNING","stg_cur":14,"mc_percent":1})"), 1000);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SystemState::Preparing), static_cast<int>(sm.state()));

  ArduinoJson::JsonDocument progress;
  SystemState s = sm.update(parsePrint(progress, R"({"gcode_state":"RUNNING","mc_percent":5})"), 2000);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SystemState::Printing), static_cast<int>(s));
}

void test_ams_status_synthesizes_filament_change() {
  PrinterStateMachine sm;
  ArduinoJson::JsonDocument doc;
  // ams_status high byte 0x01, stg_cur stays 0 (printing) — A1 behaviour.
  auto print = parsePrint(doc, R"({"gcode_state":"RUNNING","stg_cur":0,"ams_status":256,"mc_percent":40})");
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SystemState::Preparing), static_cast<int>(sm.update(print, 1000)));
}

void test_scanning_bed_is_preparing_not_leveling() {
  PrinterStateMachine sm;
  ArduinoJson::JsonDocument doc;
  auto print = parsePrint(doc, R"({"gcode_state":"PREPARE","stg_cur":9})");
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SystemState::Preparing), static_cast<int>(sm.update(print, 1000)));
}

void test_bed_level_phase_is_leveling() {
  PrinterStateMachine sm;
  ArduinoJson::JsonDocument doc;
  auto print = parsePrint(doc, R"({"gcode_state":"PREPARE","stg_cur":47})");
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SystemState::Leveling), static_cast<int>(sm.update(print, 1000)));
}

// Regression: hot nozzle during RUNNING must not rewrite the print percent bar.
void test_running_progress_not_clobbered_by_heating() {
  PrinterStateMachine sm;
  ArduinoJson::JsonDocument a;
  sm.update(parsePrint(a, R"({"gcode_state":"RUNNING","mc_percent":38,"nozzle_temper":25.0,"nozzle_target_temper":220.0})"),
            1000);
  TEST_ASSERT_EQUAL_UINT8(38, sm.telemetry().progressPercent);

  ArduinoJson::JsonDocument b;
  SystemState s = sm.update(
      parsePrint(b, R"({"gcode_state":"RUNNING","mc_percent":38,"stg_cur":14,"nozzle_temper":180.0,"nozzle_target_temper":220.0})"),
      2000);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SystemState::Preparing), static_cast<int>(s));
  TEST_ASSERT_EQUAL_UINT8(38, sm.telemetry().progressPercent);

  ArduinoJson::JsonDocument c;
  s = sm.update(
      parsePrint(c, R"({"gcode_state":"RUNNING","mc_percent":39,"stg_cur":0,"nozzle_temper":210.0,"nozzle_target_temper":220.0})"),
      3000);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SystemState::Printing), static_cast<int>(s));
  TEST_ASSERT_EQUAL_UINT8(39, sm.telemetry().progressPercent);
}

void setUp() {}
void tearDown() {}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_idle_when_no_stage_progress_or_state);
  RUN_TEST(test_printing_from_gcode_state_running);
  RUN_TEST(test_stg_cur_255_without_prior_job_is_idle);
  RUN_TEST(test_sticky_finish_on_connect_is_idle);
  RUN_TEST(test_finished_after_observed_running);
  RUN_TEST(test_paused_from_gcode_state_pause);
  RUN_TEST(test_hms_alone_does_not_force_error);
  RUN_TEST(test_print_error_triggers_error);
  RUN_TEST(test_failed_without_concrete_error_is_idle);
  RUN_TEST(test_failed_with_print_error_is_error);
  RUN_TEST(test_heating_progress_scales_to_actual_target_not_fixed_200);
  RUN_TEST(test_heating_completes_when_target_reached);
  RUN_TEST(test_printing_works_without_stg_cur_field);
  RUN_TEST(test_downloading_from_prepare_percent);
  RUN_TEST(test_clean_nozzle_stg_cur_14_is_preparing);
  RUN_TEST(test_homing_from_stg_cur_13_during_prepare);
  RUN_TEST(test_idle_gcode_ignores_homing_stage);
  RUN_TEST(test_idle_with_stg_cur_0_stays_idle);
  RUN_TEST(test_printsphere_heating_hotend_stage);
  RUN_TEST(test_printsphere_filament_change_during_pause);
  RUN_TEST(test_home_flag_does_not_force_homing);
  RUN_TEST(test_stale_prep_cleared_when_running_progress_advances);
  RUN_TEST(test_ams_status_synthesizes_filament_change);
  RUN_TEST(test_scanning_bed_is_preparing_not_leveling);
  RUN_TEST(test_bed_level_phase_is_leveling);
  RUN_TEST(test_running_progress_not_clobbered_by_heating);
  return UNITY_END();
}

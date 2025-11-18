#ifndef LOGBOOK_H
#define LOGBOOK_H

#include <Arduino.h>
#include <stddef.h>   // offsetof

// ===== Flags (coinciden con los tuyos) =====
enum JumpFlags : uint16_t {
  JF_NONE    = 0x0000,
  JF_VALID   = 0x0001,   // registro comprometido
  JF_NO_TIME = 0x0002    // sin timestamp válido
};

// ===== Registro (MISMO layout que ya usas) =====
struct __attribute__((packed)) JumpLog {
  uint32_t jump_id;          // correlativo del salto (>=1 recomendado)
  int32_t  ts_local;         // epoch local (s)
  int32_t  exit_alt_cm;      // altura salida (cm)
  int32_t  deploy_alt_cm;    // altura apertura (cm)
  uint16_t freefall_time_ds; // caída libre en decisegundos
  uint16_t vmax_ff_cmps;     // vel máx libre (cm/s)
  uint16_t vmax_can_cmps;    // vel máx bajo vela (cm/s)
  uint16_t flags;            // JF_*
  uint16_t crc16;            // CRC16 sobre [jump_id..flags]
};

// ===== API pública (igual que tu versión) =====
void     logbookInit();
bool     logbookAppend(const JumpLog& jl);
bool     logbookGetCount(uint16_t &count);   // registros válidos (<=cap)
bool     logbookGetTotal(uint32_t &total);   // total histórico (nextId-1)
bool     logbookGetByIndex(uint16_t idxNewestFirst, JumpLog &out); // idx=0 => último
bool     logbookResetAll();

// ===== Fuente de tiempo (opcional, ya la usabas) =====
typedef uint32_t (*LogbookTimeFn)();
void     logbookSetTimeSource(LogbookTimeFn fn);
uint32_t logbookNow();

// ===== Estado de sesión (sin cambios) =====
bool     logbookIsActive();
uint32_t logbookGetActiveJumpId();
float    logbookGetActiveFFTime();
void     logbookBeginFreefall(float exit_alt_m);
void     logbookMarkDeploy(float deploy_alt_m);
// Mantengo la firma para no romper includes que usan SensorMode;
// en el .cpp la tratamos como dummy int para no requerir el header del sensor.
void     logbookTick(float alt_m, int /*SensorMode dummy*/);
void     logbookFinalizeIfOpen();

#endif // LOGBOOK_H

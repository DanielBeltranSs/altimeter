// logbook.cpp  (backend LittleFS: archivo binario circular)
// API pública igual. Escrituras/crecimiento vía POSIX (O_RDWR) + lecturas POSIX.
// Trazas detalladas.

#include "logbook.h"
#include "sensor_module.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <string.h>
#include <math.h>
#include <stddef.h>
#include <esp_partition.h>

// ==== POSIX ====
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

// ==================== CONFIG DEBUG ====================
#ifndef LOGBOOK_DEBUG
#define LOGBOOK_DEBUG 1
#endif
#if LOGBOOK_DEBUG
  #define DBG(...)  do{ Serial.printf(__VA_ARGS__); }while(0)
#else
  #define DBG(...)  do{}while(0)
#endif

// Por defecto los IDs comienzan en 1 (recomendado).
// Si QUIERES que el primer salto sea id=0, descomenta la siguiente línea o
// define LOGBOOK_ID_STARTS_AT_ZERO=1 en tu build.
// #define LOGBOOK_ID_STARTS_AT_ZERO 1
// =====================================================

// ===================================================
// ==============  BACKEND LittleFS (RING)  ==========
// ===================================================

#ifndef LOGBOOK_FILE_PATH
#define LOGBOOK_FILE_PATH   "/logbook.bin"   // Ruta Arduino
#endif

#ifndef LOGBOOK_HDR_SLOT_SIZE
#define LOGBOOK_HDR_SLOT_SIZE  4096u   // 4 KiB alineado a sector (Header A en 0, B en 4096)
#endif

#ifndef LOGBOOK_CAPACITY
#define LOGBOOK_CAPACITY       30000u   // 30.000 saltos
#endif

struct __attribute__((packed)) LBHeader {
  uint32_t magic;          // "LOGB"
  uint16_t version;
  uint16_t rec_size;       // sizeof(JumpLog)
  uint32_t capacity;       // nº de registros
  uint32_t head;           // siguiente escritura [0..cap-1]
  uint32_t count;          // ocupación (0..cap)
  uint32_t nextId;         // próximo id
  uint32_t gen;            // generación (elige A/B)
  uint16_t crc;            // CRC16 de todo lo anterior
};

static const uint32_t LB_MAGIC   = 0x4C4F4742UL; // "LOGB"
static const uint16_t LB_HDR_VER = 1;

// ====== CRC-16/CCITT ======
static uint16_t crc16_ccitt(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= (uint16_t)data[i] << 8;
    for (int b = 0; b < 8; ++b) crc = (crc & 0x8000) ? ((crc<<1) ^ 0x1021) : (crc<<1);
  }
  return crc;
}
static inline uint16_t hdr_crc(const LBHeader& h) {
  return crc16_ccitt(reinterpret_cast<const uint8_t*>(&h), sizeof(LBHeader)-sizeof(h.crc));
}
static inline uint16_t rec_crc(const JumpLog& jl) {
  // CRC sobre todos los bytes previos al campo crc16 (offset 24 para tu JumpLog de 26B)
  return crc16_ccitt(reinterpret_cast<const uint8_t*>(&jl),
                     offsetof(JumpLog, crc16));
}

static inline uint32_t dataBaseOffset() { return (uint32_t)LOGBOOK_HDR_SLOT_SIZE * 2u; }

// Archivo (solo para crear/obtener size en init; lecturas via POSIX)
static File     g_file;
static LBHeader g_hdr{};
static bool     g_hdr_loaded = false;

// --- Ruta POSIX (VFS) obligatoria: usa /littlefs/...
#ifndef LOGBOOK_POSIX_PATH
#define LOGBOOK_POSIX_PATH "/littlefs" LOGBOOK_FILE_PATH
#endif
static const char* kPath = LOGBOOK_POSIX_PATH;

// ===================================================
// ============== Prototipos usados antes ============
// ===================================================
static bool ensureFS();                 // lo definimos más abajo
static void clearActiveState();         // limpia el acumulador en RAM
static void writeBothHeaders();         // escribe A y B

// ===================================================
// ==============  Helpers POSIX (escritura) =========
// ===================================================

static uint32_t posixGetSize() {
  if (!ensureFS()) return 0;
  struct stat st;
  if (::stat(kPath, &st) == 0) return (uint32_t)st.st_size;
  return 0u;
}

// Abre O_RDWR con reintento. Si errno==EIO, remonta LittleFS y reintenta una vez.
static int openRWfd_with_retry() {
  for (int att = 0; att < 2; ++att) {
    int fd = ::open(kPath, O_RDWR | O_CREAT, 0666);
    if (fd >= 0) return fd;
    int e = errno;
    DBG("[logbook] open(O_RDWR) FAIL (errno=%d %s)\n", e, strerror(e));
    if (e == EIO) {
      LittleFS.end();
      delay(10);
      if (!LittleFS.begin(false)) {
        DBG("[logbook] Remontaje LittleFS tras EIO falló\n");
        continue;
      }
      DBG("[logbook] LittleFS remontado; reintentando open()\n");
    } else {
      break;
    }
  }
  return -1;
}

static bool posixWriteAt(uint32_t off, const void* buf, size_t len) {
  if (!ensureFS()) return false;
  int fd = openRWfd_with_retry();
  if (fd < 0) return false;
  if (::lseek(fd, (off_t)off, SEEK_SET) < 0) {
    DBG("[logbook] lseek FAIL (errno=%d %s)\n", errno, strerror(errno));
    ::close(fd); return false;
  }
  ssize_t wr = ::write(fd, buf, len);
  if (wr != (ssize_t)len) {
    DBG("[logbook] pwrite FAIL (off=0x%X wr=%d len=%u errno=%d %s)\n",
        (unsigned)off, (int)wr, (unsigned)len, errno, strerror(errno));
    ::close(fd);
    return false;
  }
  ::fsync(fd);
  ::close(fd);
  return true;
}

static bool posixExtendTo(uint32_t targetSize) {
  if (!ensureFS()) return false;
  uint32_t cur = posixGetSize();
  if (cur >= targetSize) return true;

  int fd = openRWfd_with_retry();
  if (fd < 0) return false;

  if (::lseek(fd, 0, SEEK_END) < 0) {
    DBG("[logbook] lseek END FAIL (errno=%d %s)\n", errno, strerror(errno));
    ::close(fd); return false;
  }
  static uint8_t zeros[1024];
  memset(zeros, 0, sizeof(zeros));

  while (cur < targetSize) {
    uint32_t need  = targetSize - cur;
    uint32_t chunk = (need > sizeof(zeros)) ? sizeof(zeros) : need;
    ssize_t wr = ::write(fd, zeros, chunk);
    if (wr <= 0) {
      DBG("[logbook] append extend FAIL (cur=%u target=%u wr=%d errno=%d %s)\n",
          (unsigned)cur, (unsigned)targetSize, (int)wr, errno, strerror(errno));
      ::close(fd);
      return false;
    }
    cur += (uint32_t)wr;
  }
  ::fsync(fd);
  ::close(fd);
  DBG("[logbook] Archivo extendido (POSIX) a %u bytes\n", (unsigned)targetSize);
  return true;
}

static bool ensureDataCapacityPOSIX(uint32_t needSize) {
  uint32_t cur = posixGetSize();
  if (cur >= needSize) return true;
  return posixExtendTo(needSize);
}

// ===================================================
// ============== I/O posicionada (lectura) ==========
// ===================================================

static bool posixReadAt(uint32_t off, void* buf, size_t len) {
  if (len == 0) return true;
  if (!ensureFS()) return false;
  int fd = ::open(kPath, O_RDONLY);
  if (fd < 0) { DBG("[logbook] open(O_RDONLY) FAIL (errno=%d %s)\n", errno, strerror(errno)); return false; }
  if (::lseek(fd, (off_t)off, SEEK_SET) < 0) {
    DBG("[logbook] lseek(READ) FAIL (errno=%d %s)\n", errno, strerror(errno));
    ::close(fd); return false;
  }
  ssize_t rd = ::read(fd, buf, len);
  ::close(fd);
  if (rd != (ssize_t)len) {
    DBG("[logbook] read FAIL (off=0x%X len=%u rd=%d)\n", (unsigned)off, (unsigned)len, (int)rd);
    return false;
  }
  return true;
}

static bool readAt(File& /*f*/, uint32_t off, void* buf, size_t len) {
  return posixReadAt(off, buf, len);
}

// ===================================================
// ============== Header A/B ==========================
// ===================================================

static bool readHeaderSlot(uint32_t off, LBHeader& out) {
  LBHeader tmp{};
  if (!readAt(g_file, off, &tmp, sizeof(tmp))) return false;
  if (tmp.magic    != LB_MAGIC)     return false;
  if (tmp.version  != LB_HDR_VER)   return false;
  if (tmp.rec_size != sizeof(JumpLog)) return false;
  if (tmp.capacity == 0)            return false;
  if (tmp.crc      != hdr_crc(tmp)) return false;
  out = tmp; return true;
}

static bool writeHeaderSlot(uint32_t off, const LBHeader& h) {
  if (!ensureDataCapacityPOSIX(off + (uint32_t)sizeof(h))) return false;
  // Reintento simple
  for (int att = 0; att < 2; ++att) {
    if (posixWriteAt(off, &h, sizeof(h))) return true;
    delay(5);
  }
  return false;
}

static bool loadHeaderAB() {
  LBHeader A{}, B{};
  bool okA = readHeaderSlot(0, A);
  bool okB = readHeaderSlot(LOGBOOK_HDR_SLOT_SIZE, B);
  if (!okA && !okB) return false;
  g_hdr = (okA && okB) ? ((A.gen >= B.gen) ? A : B) : (okA ? A : B);
  return true;
}

// === IMPORTANTE: ahora escribimos A y B en cada actualización para evitar "retrocesos" tras power-cut
static bool storeHeaderAB() {
  g_hdr.crc = hdr_crc(g_hdr);
  bool okB = writeHeaderSlot(LOGBOOK_HDR_SLOT_SIZE, g_hdr); // B primero
  bool okA = writeHeaderSlot(0, g_hdr);                     // luego A
  if (!okA || !okB) DBG("[logbook] ERROR al escribir headers A/B (okA=%d okB=%d)\n", okA, okB);
  return okA && okB;
}

static void writeBothHeaders() {
  (void)storeHeaderAB();
}

// ===================================================
// ============== LittleFS montaje/archivo ===========
// ===================================================

static void printFSPartitionInfo() {
  const esp_partition_t* p =
    esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "spiffs");
  if (p) {
    DBG("[logbook] Partición '%s' addr=0x%X size=%u bytes\n",
        p->label, (unsigned)p->address, (unsigned)p->size);
  } else {
    DBG("[logbook] ¡Partición 'spiffs' (montada con LittleFS) NO encontrada!\n");
  }
}

static bool ensureFS() {
  static bool mounted = false;
  if (mounted) return true;
  printFSPartitionInfo();
  if (LittleFS.begin(false)) {
    mounted = true; DBG("[logbook] LittleFS montado.\n"); return true;
  }
  DBG("[logbook] LittleFS.begin(false) falló. Formateando...\n");
  LittleFS.end(); delay(50);
  if (!LittleFS.format()) { DBG("[logbook] LittleFS.format() falló.\n"); return false; }
  if (!LittleFS.begin(false)) { DBG("[logbook] LittleFS.begin tras format falló.\n"); return false; }
  mounted = true; DBG("[logbook] LittleFS formateado y montado.\n"); return true;
}

static bool reopenRead() {
  if (g_file) g_file.close();
  g_file = LittleFS.open(LOGBOOK_FILE_PATH, "r");   // SOLO lectura normal
  if (!g_file) { DBG("[logbook] reopen 'r' FALLÓ\n"); return false; }
  return true;
}

// Abre para leer; si no existe, crear vacío (w) y luego reabrir "r"
static bool ensureFileOpenRW() {
  if (g_file) return true;
  if (!ensureFS()) return false;

  g_file = LittleFS.open(LOGBOOK_FILE_PATH, "r");
  if (!g_file) {
    DBG("[logbook] creando %s ...\n", LOGBOOK_FILE_PATH);
    File f = LittleFS.open(LOGBOOK_FILE_PATH, "w"); // crea/trunca
    if (!f) { DBG("[logbook] ERROR creando archivo.\n"); return false; }
    f.close();
    g_file = LittleFS.open(LOGBOOK_FILE_PATH, "r");
    if (!g_file) { DBG("[logbook] ERROR reabriendo en 'r'.\n"); return false; }
  }
  return (bool)g_file;
}

static void formatFreshFile(uint32_t capacity) {
  // 1) Truncar archivo con "w"
  {
    File fw = LittleFS.open(LOGBOOK_FILE_PATH, "w");
    if (!fw) { DBG("[logbook] NO se pudo truncar/crear con 'w'\n"); }
    fw.close();
  }

  // 2) Preparar header
  memset(&g_hdr, 0, sizeof(g_hdr));
  g_hdr.magic    = LB_MAGIC;
  g_hdr.version  = LB_HDR_VER;
  g_hdr.rec_size = sizeof(JumpLog);
  g_hdr.capacity = capacity;
  g_hdr.head     = 0;
  g_hdr.count    = 0;
#ifdef LOGBOOK_ID_STARTS_AT_ZERO
  g_hdr.nextId   = 0;
#else
  g_hdr.nextId   = 1;
#endif
  g_hdr.gen      = 1;
  g_hdr.crc      = hdr_crc(g_hdr);

  // 3) Opción A: NO preasignar la región de datos para no agotar bloques libres.
  //    (Solo aseguramos que existen los headers A/B)
  (void)ensureDataCapacityPOSIX(dataBaseOffset());

  // 4) Escribir ambos headers (POSIX)
  writeBothHeaders();

  // 5) Reabrir archivo en "r" para lecturas
  reopenRead();

  g_hdr_loaded = true;
  DBG("[logbook] Archivo nuevo: cap=%u rec=%u bytes base=0x%X size=%u\n",
      (unsigned)g_hdr.capacity, (unsigned)g_hdr.rec_size,
      (unsigned)dataBaseOffset(), (unsigned)g_file.size());
}

// ===================================================
// ============  REPARACIÓN RÁPIDA DE COLA ===========
// ===================================================

// Revisa hasta 'maxProbe' slots hacia atrás desde head-1; si encuentra inválidos consecutivos,
// retrocede head y reduce count en memoria, luego persiste headers. Evita escaneo completo.
static void quickFixTailSlots(uint32_t maxProbe = 4) {
  if (!g_hdr_loaded) return;
  if (g_hdr.count == 0) return;
  uint32_t fixed = 0;
  JumpLog tmp{};
  while (fixed < maxProbe && g_hdr.count > 0) {
    uint32_t last = (g_hdr.head == 0) ? (g_hdr.capacity - 1) : (g_hdr.head - 1);
    uint32_t off  = dataBaseOffset() + last * g_hdr.rec_size;
    if (!posixReadAt(off, &tmp, sizeof(tmp))) break;
    uint16_t exp = rec_crc(tmp);
    if ((tmp.flags & JF_VALID) && exp == tmp.crc16) break; // ya está bien
    // registro inválido => retrocede ring
    g_hdr.head = last;
    g_hdr.count--;
    fixed++;
  }
  if (fixed) { g_hdr.gen++; storeHeaderAB(); }
}

// ===================================================
// ============  API PÚBLICA (persistencia) ==========
// ===================================================

void logbookInit() {
  if (!ensureFileOpenRW()) { Serial.println("[logbook] LittleFS no montó / no abrió."); return; }

  DBG("[logbook] schema: sizeof(JumpLog)=%u crcOff=%u\n",
      (unsigned)sizeof(JumpLog), (unsigned)offsetof(JumpLog, crc16));

  if (!loadHeaderAB()) {
    Serial.println("[logbook] Formateando archivo de bitácora...");
    formatFreshFile(LOGBOOK_CAPACITY);
  } else {
    if (g_hdr.version != LB_HDR_VER || g_hdr.rec_size != sizeof(JumpLog)) {
      Serial.println("[logbook] Header incompatible → reformateando archivo.");
      formatFreshFile(LOGBOOK_CAPACITY);
    } else {
      // Conciliación de capacidad: expandir en caliente (sin reformatear)
      if (g_hdr.capacity != LOGBOOK_CAPACITY) {
        uint32_t oldCap = g_hdr.capacity;
        uint32_t newCap = LOGBOOK_CAPACITY;
        if (newCap > oldCap) {
          // Opción A => NO preasignar, solo headers
          uint32_t need = dataBaseOffset(); // solo headers
          if (ensureDataCapacityPOSIX(need)) {
            g_hdr.capacity = newCap;
            g_hdr.gen++;
            writeBothHeaders();
            DBG("[logbook] Capacidad ampliada old=%u -> new=%u\n", (unsigned)oldCap, (unsigned)newCap);
          } else {
            DBG("[logbook] ERROR expandiendo archivo; se mantiene capacidad=%u\n", (unsigned)oldCap);
          }
        } else {
          Serial.println("[logbook] Capacidad menor solicitada → reformateando archivo.");
          formatFreshFile(newCap);
          return;
        }
      }

      // Opción A: no preasignar; solo garantizamos que existen los headers
      (void)ensureDataCapacityPOSIX(dataBaseOffset());

#ifndef LOGBOOK_ID_STARTS_AT_ZERO
      if (g_hdr.nextId == 0) {
        DBG("[logbook] Corrigiendo nextId=0 -> 1\n");
        g_hdr.nextId = 1;
        g_hdr.gen++;
        (void)storeHeaderAB();
      }
#endif

      // Reparación rápida de cola (por si el último commit quedó a medias)
      g_hdr_loaded = true;
      quickFixTailSlots();

      DBG("[logbook] Header OK: head=%u count=%u nextId=%u gen=%u size=%u\n",
          (unsigned)g_hdr.head, (unsigned)g_hdr.count,
          (unsigned)g_hdr.nextId, (unsigned)g_hdr.gen, (unsigned)g_file.size());
    }
  }
}

bool logbookAppend(const JumpLog& jl_in) {
  if (!g_hdr_loaded) return false;

  uint32_t pos  = g_hdr.head % g_hdr.capacity;
  uint32_t off  = dataBaseOffset() + pos * g_hdr.rec_size;

  // Asegura capacidad hasta el final del registro (crecimiento on-demand)
  if (!ensureDataCapacityPOSIX(off + (uint32_t)sizeof(JumpLog))) {
    DBG("[logbook] ensure capacity FAIL (off+size=%u)\n", (unsigned)(off + (uint32_t)sizeof(JumpLog)));
    return false;
  }

  // === Commit en 2 fases ===
  JumpLog rec = jl_in;

  // Paso 1: preparar SIN JF_VALID y con CRC coherente
  rec.flags = (uint16_t)(rec.flags & ~JF_VALID);
  rec.crc16 = rec_crc(rec);
  bool wrok = false;
  for (int att = 0; att < 2 && !wrok; ++att) {
    wrok = posixWriteAt(off, &rec, sizeof(rec));
    if (!wrok) delay(5);
  }
  if (!wrok) {
    DBG("[logbook] ERROR posixWriteAt(off=0x%X sizeNow=%u)\n", (unsigned)off, (unsigned)posixGetSize());
    return false;
  }

  // Paso 2: marcar commit => set JF_VALID y actualizar CRC16 in-place
  uint16_t flags2 = (uint16_t)(rec.flags | JF_VALID);
  uint16_t crc2;
  {
    JumpLog tmp2 = rec; tmp2.flags = flags2; tmp2.crc16 = 0;
    crc2 = rec_crc(tmp2);
  }
  // Escribir FLAGS
  if (!posixWriteAt(off + (uint32_t)offsetof(JumpLog, flags), &flags2, sizeof(flags2))) return false;
  // Escribir CRC
  if (!posixWriteAt(off + (uint32_t)offsetof(JumpLog, crc16), &crc2, sizeof(crc2))) return false;

  // Actualiza header en RAM y persiste A/B
  g_hdr.head = (pos + 1) % g_hdr.capacity;
  if (g_hdr.count < g_hdr.capacity) g_hdr.count++;
  if (rec.jump_id + 1 > g_hdr.nextId) g_hdr.nextId = rec.jump_id + 1;
  g_hdr.gen++;

  bool ok = storeHeaderAB();  // escribe A y B
  DBG("[logbook] append %s id=%lu pos=%u count=%u next=%u gen=%u fileSize=%u\n",
      ok?"ok":"FAIL",
      (unsigned long)rec.jump_id, (unsigned)pos,
      (unsigned)g_hdr.count, (unsigned)g_hdr.nextId, (unsigned)g_hdr.gen,
      (unsigned)posixGetSize());
  return ok;
}

bool logbookGetCount(uint16_t &count) {
  count = g_hdr_loaded ? (uint16_t)g_hdr.count : 0;
  return g_hdr_loaded;
}
bool logbookGetTotal(uint32_t &total) {
  total = g_hdr_loaded ? ((g_hdr.nextId > 0) ? (g_hdr.nextId - 1) : 0) : 0;
  return g_hdr_loaded;
}

bool logbookGetByIndex(uint16_t idxNewestFirst, JumpLog &out) {
  if (!g_hdr_loaded || g_hdr.count == 0) return false;
  if (idxNewestFirst >= g_hdr.count)     return false;

  uint32_t last = (g_hdr.head == 0) ? (g_hdr.capacity - 1) : (g_hdr.head - 1);
  uint32_t pos  = (last + g_hdr.capacity - idxNewestFirst) % g_hdr.capacity;
  uint32_t off  = dataBaseOffset() + pos * g_hdr.rec_size;

  if (!readAt(g_file, off, &out, sizeof(out))) {
    DBG("[logbook] ERROR readAt(off=0x%X)\n", (unsigned)off);
    return false;
  }

  uint16_t expect = rec_crc(out);
  if (expect != out.crc16) {
    DBG("[logbook] CRC BAD en pos=%u (got=0x%04X exp=0x%04X)\n",
        (unsigned)pos, out.crc16, expect);
    return false;
  }
  if (!(out.flags & JF_VALID)) {
    DBG("[logbook] registro no comprometido en pos=%u\n", (unsigned)pos);
    return false;
  }
  return true;
}

// ====== BORRADO COMPLETO DEL ÁREA DE REGISTROS ======
static bool posixZeroRange(uint32_t off, uint32_t len) {
  if (len == 0) return true;
  if (!ensureFS()) return false;
  int fd = openRWfd_with_retry();
  if (fd < 0) return false;
  if (::lseek(fd, (off_t)off, SEEK_SET) < 0) {
    DBG("[logbook] lseek(ZERO) FAIL (errno=%d %s)\n", errno, strerror(errno));
    ::close(fd); return false;
  }
  static uint8_t zeros[1024];
  memset(zeros, 0, sizeof(zeros));
  uint32_t left = len;
  while (left > 0) {
    uint32_t chunk = (left > sizeof(zeros)) ? sizeof(zeros) : left;
    ssize_t wr = ::write(fd, zeros, chunk);
    if (wr != (ssize_t)chunk) {
      DBG("[logbook] zero-range FAIL (wr=%d chunk=%u errno=%d %s)\n",
          (int)wr, (unsigned)chunk, errno, strerror(errno));
      ::close(fd); return false;
    }
    left -= (uint32_t)wr;
  }
  ::fsync(fd);
  ::close(fd);
  return true;
}

bool logbookResetAll() {
  if (!g_hdr_loaded) return false;
  clearActiveState();
  formatFreshFile(g_hdr.capacity);  // trunca y escribe headers A/B frescos
  DBG("[logbook] reset ok (fresh format; cap=%u)\n", (unsigned)g_hdr.capacity);
  return true;
}



// ===================================================
// ==============  TIME SOURCE (opcional) ============
// ===================================================
static LogbookTimeFn s_timefn = nullptr;
void logbookSetTimeSource(LogbookTimeFn fn) { s_timefn = fn; }
uint32_t logbookNow() { return s_timefn ? s_timefn() : 0; }

// ===================================================
// ==============  ACUMULADOR EN RAM  ================
// ===================================================
static bool     s_active          = false;
static bool     s_ffClosed        = false;
static uint32_t s_startMs         = 0;
static uint32_t s_ffStartMs       = 0;
static uint32_t s_ffEndMs         = 0;
static int32_t  s_exitAlt_cm      = 0;
static int32_t  s_deployAlt_cm    = 0;
static uint16_t s_vmaxFF_cmps     = 0;
static uint16_t s_vmaxCan_cmps    = 0;
static uint32_t s_activeId        = 0;

static float    s_prevAlt_m       = NAN;
static uint32_t s_prevMs          = 0;
static int32_t  s_tStartEpoch     = 0;

// Helper para dejar el estado limpio
static void clearActiveState() {
  s_active       = false;
  s_ffClosed     = false;
  s_prevAlt_m    = NAN;
  s_prevMs       = 0;
  s_tStartEpoch  = 0;
  s_activeId     = 0;
}

// Helpers numéricos
static inline uint16_t to_cmps_mag(float vz_mps) {
  float mag = fabsf(vz_mps);
  long  vcm = lroundf(mag * 100.0f);
  if (vcm < 0) vcm = 0; if (vcm > 65535) vcm = 65535;
  return (uint16_t)vcm;
}
static inline int32_t to_cm(float m) {
  long v = lroundf(m * 100.0f);
  if (v >  2147483647L) v = 2147483647L;
  if (v < -2147483647L) v = -2147483647L;
  return (int32_t)v;
}
static inline uint16_t to_ds(uint32_t ms) {
  uint32_t ds = (ms + 50) / 100;
  if (ds > 65535U) ds = 65535U;
  return (uint16_t)ds;
}

// Estado/consulta activa
bool     logbookIsActive()        { return s_active; }
uint32_t logbookGetActiveJumpId() { return s_activeId; }

float logbookGetActiveFFTime() {
  if (!s_active) return 0.0f;
  uint32_t now = millis();
  uint32_t endMs = s_ffClosed ? s_ffEndMs : now;
  if (s_ffStartMs == 0) return 0.0f;
  uint32_t dt = (endMs > s_ffStartMs) ? (endMs - s_ffStartMs) : 0;
  return dt / 1000.0f;
}

void logbookBeginFreefall(float exit_alt_m) {
  s_active       = true;
  s_ffClosed     = false;
  s_startMs      = millis();
  s_ffStartMs    = s_startMs;
  s_ffEndMs      = 0;
  s_exitAlt_cm   = to_cm(exit_alt_m);
  s_deployAlt_cm = 0;
  s_vmaxFF_cmps  = 0;
  s_vmaxCan_cmps = 0;
  s_prevAlt_m    = NAN;
  s_prevMs       = 0;

  s_tStartEpoch  = (int32_t)logbookNow();
  s_activeId     = g_hdr.nextId;

  DBG("[logbook] begin FF id(tent)=%u exit=%.2f m epoch=%ld\n",
      (unsigned)g_hdr.nextId, exit_alt_m, (long)s_tStartEpoch);
}

void logbookMarkDeploy(float deploy_alt_m) {
  if (!s_active || s_ffClosed) return;
  s_deployAlt_cm = to_cm(deploy_alt_m);
  s_ffEndMs      = millis();
  s_ffClosed     = true;
  DBG("[logbook] deploy alt=%.2f m\n", deploy_alt_m);
}

void logbookTick(float alt_m, int /*SensorMode*/) {
  uint32_t now = millis();
  if (!isfinite(s_prevAlt_m)) { s_prevAlt_m = alt_m; s_prevMs = now; return; }
  uint32_t dt_ms = now - s_prevMs; if (dt_ms < 20) return;
  float dt = dt_ms / 1000.0f;
  float vz = (alt_m - s_prevAlt_m) / dt;

  if (s_active) {
    uint16_t vcm = to_cmps_mag(vz);
    if (!s_ffClosed) { if (vcm > s_vmaxFF_cmps)  s_vmaxFF_cmps  = vcm; }
    else             { if (vcm > s_vmaxCan_cmps) s_vmaxCan_cmps = vcm; }
  }
  s_prevAlt_m = alt_m; s_prevMs = now;
}

void logbookFinalizeIfOpen() {
  if (!s_active) return;

  if (!s_ffClosed) {
    s_deployAlt_cm = to_cm(s_prevAlt_m);
    s_ffEndMs      = millis();
    s_ffClosed     = true;
  }

  uint32_t ff_ms = (s_ffEndMs > s_ffStartMs) ? (s_ffEndMs - s_ffStartMs) : 0;

  if (ff_ms < 500U) {
    DBG("[logbook] cancel micro-salto (%ums)\n", (unsigned)ff_ms);
    s_active=false; s_ffClosed=false; s_prevAlt_m=NAN; s_prevMs=0; s_tStartEpoch=0;
    return;
  }

  JumpLog jl{};
  jl.jump_id          = g_hdr.nextId;
  jl.ts_local         = s_tStartEpoch;
  jl.exit_alt_cm      = s_exitAlt_cm;
  jl.deploy_alt_cm    = s_deployAlt_cm;
  jl.freefall_time_ds = to_ds(ff_ms);
  jl.vmax_ff_cmps     = s_vmaxFF_cmps;
  jl.vmax_can_cmps    = s_vmaxCan_cmps;
  jl.flags            = (uint16_t)(JF_VALID);
  if (jl.ts_local == 0) jl.flags |= JF_NO_TIME;
  jl.crc16 = 0; // se recalcula en append

  (void)logbookAppend(jl);   // avanza head/count/nextId/gen y guarda

  // Limpia estado activo
  s_active      = false;
  s_ffClosed    = false;
  s_prevAlt_m   = NAN;
  s_prevMs      = 0;
  s_tStartEpoch = 0;
}

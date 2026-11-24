/*
 * AirLink C API: the Rust core (core/airlink-core) as a C library.
 *
 * Model
 *  - One AirlinkNode per process. All networking runs on the library's own threads.
 *  - Nothing blocks except airlink_start / airlink_stop. Slow operations return an
 *    operation id at once; their outcome arrives later as an event.
 *  - Events are JSON objects, read with airlink_poll_event (non-blocking, any thread;
 *    the app polls it from its UI loop). Each has an "event" field:
 *
 *      Node events (see airlink_core::Event):
 *        peer_discovered, peer_lost, pairing_request, pairing_completed, request_cancelled,
 *        file_offer, transfer_pending, transfer_progress, transfer_completed,
 *        transfer_failed, clipboard_received, app_message, media_started, media_stopped
 *      desktop_started, desktop_stopped: this computer's screen share (airlink_desktop_start)
 *      request_answered {"request_id","accepted"}: airlink_respond took an answer
 *      Operation results:
 *        {"event":"op_result","op":N,"ok":true,"value":...}
 *        {"event":"op_result","op":N,"ok":false,"error":"..."}
 *        {"event":"pairing_code","op":N,"pairing":P,"code":"123456","peer":{...}}
 *
 *  - Every char* returned by the library must be released with airlink_free_string.
 *  - Strings passed in are UTF-8, NUL-terminated, and only read during the call.
 */
#ifndef AIRLINK_H
#define AIRLINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AirlinkNode AirlinkNode;

/* Starts the node. config_json:
 *   {"data_dir": "...", "download_dir": "..." (both optional: the per-user data dir and
 *    Downloads/AirLink by default),
 *    "device_name": "..." (optional, renames), "fallback_name": "..." (optional, used
 *    only when no name was saved before, e.g. the phone's model)}
 * Returns NULL on failure and, if error_out is not NULL, sets *error_out to a message. */
AirlinkNode* airlink_start(const char* config_json, char** error_out);

/* Shuts down cleanly (peers are told at once) and frees the node. */
void airlink_stop(AirlinkNode* node);

/* Next pending event as JSON, or NULL when there is none. */
char* airlink_poll_event(AirlinkNode* node);

/* {"id","name","platform","port","download_dir"} */
char* airlink_info(AirlinkNode* node);

/* [{"id","name","platform","paired":bool,"online":bool}, ...] sorted by name. */
char* airlink_peers(AirlinkNode* node);

/* Answers a pairing_request or file_offer event. False if it is no longer pending. */
bool airlink_respond(AirlinkNode* node, uint64_t request_id, bool accept);

/* Hears, on the library's own thread and as they happen, the events that ask the user something
 * or take such a question back: pairing_request, file_offer, request_cancelled and
 * request_answered (the JSON is only valid during the call). For showing them while the app's UI
 * is not running (Android's notifications); they are still queued for airlink_poll_event too.
 * NULL removes it. */
typedef void (*airlink_prompt_fn)(void* ctx, const char* event_json);
void airlink_set_prompt_listener(AirlinkNode* node, airlink_prompt_fn listener, void* ctx);

/* Removes a paired device. */
bool airlink_forget(AirlinkNode* node, const char* peer_id);

/* Starts pairing with a discovered device. Result: a pairing_code event (show the code,
 * then call airlink_confirm_pairing) or a failed op_result. */
uint64_t airlink_start_pairing(AirlinkNode* node, const char* peer_id);

/* Starts pairing by numeric IP, optionally :port (default 47820), when mDNS is unavailable.
 * Result: pairing_code or a failed op_result. */
uint64_t airlink_start_pairing_address(AirlinkNode* node, const char* address);

/* Sends the local decision for a pairing_code. Result: op_result, value = true when both
 * sides accepted. */
uint64_t airlink_confirm_pairing(AirlinkNode* node, uint64_t pairing, bool accept);

/* paths_json: ["/path/a", "/path/dir", ...]. Result: op_result (value = transfer id);
 * progress comes as transfer_* events. */
uint64_t airlink_send_files(AirlinkNode* node, const char* peer_id, const char* paths_json);

/* Result: op_result. */
uint64_t airlink_send_clipboard(AirlinkNode* node, const char* peer_id, const char* text);

/* An app message to a paired device (see docs/protocol.md, app_message): `kind` names it
 * ("notification.posted"...), body_json is any JSON value. Result: op_result. The other side gets
 * an app_message event {"peer", "kind", "body"}. */
uint64_t airlink_send_message(AirlinkNode* node, const char* peer_id, const char* kind, const char* body_json);

/* Media: binary frames (JPEG or ALH1-prefixed independent H.264 access units) on a stream `name`. Sending
 * returns at once; a frame not yet written when the next is sent is dropped (the latest wins).
 * The receiver hears media_started / media_stopped {"peer_id","name"} events and takes the newest
 * frame with airlink_media_take (false: none new since the last take; free it with
 * airlink_free_bytes). */
bool airlink_media_send(AirlinkNode* node, const char* peer_id, const char* name, const uint8_t* data, size_t len);
void airlink_media_stop(AirlinkNode* node, const char* peer_id, const char* name);
bool airlink_media_take(AirlinkNode* node, const char* peer_id, const char* name, uint8_t** data, size_t* len);
void airlink_free_bytes(uint8_t* data, size_t len);

/* Sharing this computer's screen with a paired device (phase 4; Linux and Windows).
 * The device asks with a "desktop.start" app message; the app decides (a prompt, a trusted
 * device) and calls airlink_desktop_start. Frames go on the media stream "desktop" as ALH1
 * H.264 key frames; the device's "desktop.input" messages are played with airlink_desktop_input.
 * Linux asks the desktop once (xdg-desktop-portal RemoteDesktop); the grant is kept in the data
 * dir. One device at a time: a new start ends the previous share (reason "replaced").
 *   airlink_desktop_start: options_json {"max_width":1280,"fps":12} (NULL: defaults).
 *     Result: op_result (value = true when sharing), then the events
 *     {"event":"desktop_started","peer_id","width","height"} or
 *     {"event":"desktop_stopped","peer_id","reason","error"}; the device hears desktop.started
 *     / desktop.stopped.
 *   airlink_desktop_stop: peer_id NULL stops any share; reason NULL is "computer". */
bool airlink_desktop_supported(void);
uint64_t airlink_desktop_start(AirlinkNode* node, const char* peer_id, const char* options_json);
void airlink_desktop_stop(AirlinkNode* node, const char* peer_id, const char* reason);
bool airlink_desktop_input(AirlinkNode* node, const char* peer_id, const char* body_json);

/* Decodes an ALH1 H.264 frame into RGBA. Decoder state is local to one stream; free it after
 * the stream ends. On success, pixels is width * height * 4 bytes and must be freed with
 * airlink_free_bytes. Returns false for invalid packets or frames without a picture. */
typedef struct AirlinkH264Decoder AirlinkH264Decoder;
AirlinkH264Decoder* airlink_h264_decoder_new(void);
bool airlink_h264_decode(AirlinkH264Decoder* decoder, const uint8_t* packet, size_t len,
                         uint8_t** pixels, size_t* pixels_len, int* width, int* height);
void airlink_h264_decoder_free(AirlinkH264Decoder* decoder);

/* Advanced mode (Android): the phone pairs with its own wireless debugging once (the six-digit
 * code of "Pair device with pairing code"), then runs shell commands as adbd's shell user. `dir`
 * keeps the key adbd trusts. Results: op_result (for shell, value = its standard output). */
uint64_t airlink_adb_pair(AirlinkNode* node, const char* dir, const char* code);
/* As airlink_adb_pair, using the port shown on the six-digit pairing screen.
 * A port of 0 uses mDNS discovery. */
uint64_t airlink_adb_pair_port(AirlinkNode* node, const char* dir, const char* code, uint16_t port);
uint64_t airlink_adb_shell(AirlinkNode* node, const char* dir, const char* command);
/* As airlink_adb_shell, using the TCP port shown on Android's Wireless debugging screen.
 * A port of 0 uses the normal mDNS discovery path. */
uint64_t airlink_adb_shell_port(AirlinkNode* node, const char* dir, const char* command, uint16_t port);

void airlink_free_string(char* s);

#ifdef __cplusplus
}
#endif

#endif /* AIRLINK_H */

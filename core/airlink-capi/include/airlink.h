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
 *        transfer_failed, clipboard_received
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

/* Sends the local decision for a pairing_code. Result: op_result, value = true when both
 * sides accepted. */
uint64_t airlink_confirm_pairing(AirlinkNode* node, uint64_t pairing, bool accept);

/* paths_json: ["/path/a", "/path/dir", ...]. Result: op_result (value = transfer id);
 * progress comes as transfer_* events. */
uint64_t airlink_send_files(AirlinkNode* node, const char* peer_id, const char* paths_json);

/* Result: op_result. */
uint64_t airlink_send_clipboard(AirlinkNode* node, const char* peer_id, const char* text);

void airlink_free_string(char* s);

#ifdef __cplusplus
}
#endif

#endif /* AIRLINK_H */

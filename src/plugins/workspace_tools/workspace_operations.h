/**
 * @file workspace_operations.h
 * @brief Function pointer definitions for workspace operations (implemented in Rust)
 *
 * This header defines the interface between the C workspace tools plugin and
 * the Rust implementation. Function pointers are passed from Rust during
 * initialization to enable the C tools to call back into Rust workspace logic.
 */

#ifndef ETHERVOX_WORKSPACE_OPERATIONS_H
#define ETHERVOX_WORKSPACE_OPERATIONS_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief List all objects in the workspace
 *
 * @param type_filter Optional type filter (e.g., "markdown-note", "folder", "all")
 * @param result Output parameter for JSON array of objects
 * @param error Output parameter for error messages
 * @return 0 on success, -1 on error
 */
typedef int (*workspace_list_objects_fn)(const char* type_filter, char** result, char** error);

/**
 * @brief Search workspace objects by query
 *
 * @param query Search query string
 * @param result Output parameter for JSON array of matching objects
 * @param error Output parameter for error messages
 * @return 0 on success, -1 on error
 */
typedef int (*workspace_search_objects_fn)(const char* query, char** result, char** error);

/**
 * @brief Get complete details of a specific object
 *
 * @param object_id UUID of the object to retrieve
 * @param result Output parameter for JSON object with full details
 * @param error Output parameter for error messages
 * @return 0 on success, -1 on error
 */
typedef int (*workspace_get_object_fn)(const char* object_id, char** result, char** error);

/**
 * @brief Create a new markdown note
 *
 * @param title Title of the note
 * @param content Markdown content
 * @param tags_json JSON array of tags (optional, can be NULL)
 * @param result Output parameter for JSON object with created note details
 * @param error Output parameter for error messages
 * @return 0 on success, -1 on error
 */
typedef int (*workspace_create_note_fn)(const char* title, const char* content,
                                        const char* tags_json, char** result, char** error);

/**
 * @brief Create a connection between two objects
 *
 * @param from_id Source object UUID
 * @param to_id Target object UUID
 * @param label Optional connection label (can be NULL)
 * @param result Output parameter for connection ID
 * @param error Output parameter for error messages
 * @return 0 on success, -1 on error
 */
typedef int (*workspace_create_connection_fn)(const char* from_id, const char* to_id,
                                              const char* label, char** result, char** error);

/**
 * @brief Collection of workspace operation function pointers
 *
 * This struct is populated by Rust and passed to the C plugin during
 * registration. It provides all the callbacks needed for workspace operations.
 */
typedef struct {
  workspace_list_objects_fn list_objects;
  workspace_search_objects_fn search_objects;
  workspace_get_object_fn get_object;
  workspace_create_note_fn create_note;
  workspace_create_connection_fn create_connection;
} workspace_operations_t;

#ifdef __cplusplus
}
#endif

#endif /* ETHERVOX_WORKSPACE_OPERATIONS_H */

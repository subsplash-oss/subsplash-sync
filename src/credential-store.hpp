#pragma once

#include <QString>

/*
 * Secure storage for Subsplash API credentials, backed by the OS-native
 * credential store (macOS Keychain / Windows Credential Manager / Linux Secret
 * Service) via QtKeychain. This replaces persisting secrets in config.json.
 *
 * All calls are synchronous: each spins a nested event loop until the
 * underlying QtKeychain job finishes, so they must be invoked from the Qt
 * (GUI) thread.
 */
namespace cred_store {

/* Keys under which the three credentials are stored. */
extern const QString kClientId;
extern const QString kClientSecret;
extern const QString kAppKey;

/*
 * Read the secret stored under `key`. On success returns true and sets
 * `out_value`. Returns false if the entry is absent or on error; when
 * `out_error` is non-null it receives a human-readable message for hard errors
 * (it is left untouched for a plain "not found").
 */
bool Read(const QString &key, QString &out_value, QString *out_error = nullptr);

/* Store (or overwrite) the secret under `key`. Returns true on success. */
bool Write(const QString &key, const QString &value, QString *out_error = nullptr);

/*
 * Remove the secret stored under `key`. Returns true on success or if the
 * entry was already absent.
 */
bool Remove(const QString &key, QString *out_error = nullptr);

} // namespace cred_store

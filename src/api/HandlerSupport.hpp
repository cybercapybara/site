/**
 * @file HandlerSupport.hpp
 * @brief Shared controller-side helpers: map repository exceptions to the
 *        canonical HTTP error responses in one place.
 * @details Every mutating handler used to repeat the same try/catch ladder
 *          (DuplicateEmail->409, *NotFound->404, RoleInUse->409, std::exception
 *          ->500). That's ~13 copies that drift in their codes/messages and
 *          that a new handler can forget. with_repo_errors() centralizes the
 *          mapping so the error contract is defined once.
 *
 *          The repository layer still owns the BOUNDARY (it throws typed
 *          exceptions, it does not know about HTTP); this helper is the api-
 *          side translation of those types — the natural counterpart to
 *          Repositories::detail::translate_sql (SQLSTATE -> exception).
 */

#pragma once

#include <exception>
#include <functional>

#include <drogon/HttpResponse.h>
#include <spdlog/spdlog.h>

#include "repositories/RoleRepository.hpp"
#include "repositories/SqlErrors.hpp"
#include "repositories/UserRepository.hpp"
#include "utils/ErrorResponse.hpp"

namespace Api {

/**
 * @brief Run @p fn, translating any repository exception into the canonical
 *        HTTP error response via @p cb. @p op is a short label for the 500
 *        log line ("admin createUser"). Returns true if @p fn completed
 *        without throwing — handlers that must keep running after the guarded
 *        block (e.g. to send a success response) can branch on it.
 *
 * Codes are the stable machine codes asserted by tests:
 *   DuplicateEmail -> 409 email_taken | UserNotFound -> 404 user
 *   DuplicateRole  -> 409 role_exists | RoleNotFound -> 404 role
 *   RoleInUse      -> 409 role_in_use | anything else -> 500
 */
template <typename Fn>
inline bool with_repo_errors(const std::function<void(const drogon::HttpResponsePtr&)>& cb, const char* op, Fn&& fn) {
    try {
        fn();
        return true;
    } catch (const Repositories::DuplicateEmail&) {
        cb(ErrorResponse::conflict("email_taken", "Email is already registered"));
    } catch (const Repositories::UserNotFound&) {
        cb(ErrorResponse::not_found("user"));
    } catch (const Repositories::DuplicateRole&) {
        cb(ErrorResponse::conflict("role_exists", "A role with that name already exists"));
    } catch (const Repositories::RoleNotFound&) {
        cb(ErrorResponse::not_found("role"));
    } catch (const Repositories::RoleInUse&) {
        cb(ErrorResponse::conflict("role_in_use", "Reassign users away from this role before deleting"));
    } catch (const std::exception& e) {
        spdlog::error("{} failed: {}", op, e.what());
        cb(ErrorResponse::internal_error());
    }
    return false;
}

}  // namespace Api

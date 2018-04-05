/**
 *    Copyright (C) 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/commands/write_commands/write_commands_common.h"

#include <algorithm>
#include <string>
#include <vector>

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace auth {
namespace {

/**
 * Helper to extract the namespace being indexed from a raw BSON write command.
 *
 * TODO: Remove when we have parsing hooked before authorization.
 */
NamespaceString getIndexedNss(const std::vector<BSONObj>& documentsToInsert) {
    uassert(ErrorCodes::FailedToParse, "index write batch is empty", !documentsToInsert.empty());
    std::string nsToIndex = documentsToInsert.front()["ns"].str();
    uassert(ErrorCodes::FailedToParse,
            "index write batch contains an invalid index descriptor",
            !nsToIndex.empty());
    uassert(ErrorCodes::FailedToParse,
            "index write batches may only contain a single index descriptor",
            documentsToInsert.size() == 1);
    return NamespaceString(std::move(nsToIndex));
}

}  // namespace

void checkAuthForWriteCommand(AuthorizationSession* authzSession,
                              BatchedCommandRequest::BatchType cmdType,
                              const OpMsgRequest& request) {
    std::vector<Privilege> privileges;
    ActionSet actionsOnCommandNSS;

    if (shouldBypassDocumentValidationForCommand(request.body)) {
        actionsOnCommandNSS.addAction(ActionType::bypassDocumentValidation);
    }

    NamespaceString cmdNSS;

    switch (cmdType) {
        case BatchedCommandRequest::BatchType_Insert: {
            auto op = write_ops::Insert::parse(IDLParserErrorContext("insert"), request);
            cmdNSS = op.getNamespace();
            if (!op.getNamespace().isSystemDotIndexes()) {
                actionsOnCommandNSS.addAction(ActionType::insert);
            } else {
                // Special-case indexes until we have a command
                auto nssToIndex = getIndexedNss(op.getDocuments());
                privileges.push_back(Privilege(ResourcePattern::forExactNamespace(nssToIndex),
                                               ActionType::createIndex));
            }
            break;
        }
        case BatchedCommandRequest::BatchType_Update: {
            auto op = write_ops::Update::parse(IDLParserErrorContext("update"), request);
            cmdNSS = op.getNamespace();
            actionsOnCommandNSS.addAction(ActionType::update);
            // Upsert also requires insert privs
            const auto& updates = op.getUpdates();
            if (std::any_of(
                    updates.begin(), updates.end(), [](auto&& x) { return x.getUpsert(); })) {
                actionsOnCommandNSS.addAction(ActionType::insert);
            }
            break;
        }
        case BatchedCommandRequest::BatchType_Delete: {
            auto op = write_ops::Delete::parse(IDLParserErrorContext("delete"), request);
            cmdNSS = op.getNamespace();
            actionsOnCommandNSS.addAction(ActionType::remove);
            break;
        }
    }

    if (!actionsOnCommandNSS.empty()) {
        privileges.push_back(
            Privilege(ResourcePattern::forExactNamespace(cmdNSS), actionsOnCommandNSS));
    }

    uassert(ErrorCodes::Unauthorized,
            "unauthorized",
            authzSession->isAuthorizedForPrivileges(privileges));
}

}  // namespace auth
}  // namespace mongo

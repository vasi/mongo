/**
 *    Copyright (C) 2020-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/sharding_ddl_util.h"

#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/grid.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"

namespace mongo {

namespace sharding_ddl_util {

void shardedRenameMetadata(OperationContext* opCtx,
                           const NamespaceString& fromNss,
                           const NamespaceString& toNss) {
    // TODO SERVER-53871: enclose the following operations into a transaction
    auto catalogClient = Grid::get(opCtx)->catalogClient();
    auto collType = catalogClient->getCollection(opCtx, fromNss);

    // Delete the FROM collection entry
    uassertStatusOK(
        catalogClient->removeConfigDocuments(opCtx,
                                             CollectionType::ConfigNS,
                                             BSON(CollectionType::kNssFieldName << fromNss.ns()),
                                             ShardingCatalogClient::kMajorityWriteConcern));

    collType.setNss(toNss);
    // Insert the TO collection entry
    uassertStatusOK(
        catalogClient->insertConfigDocument(opCtx,
                                            CollectionType::ConfigNS,
                                            collType.toBSON(),
                                            ShardingCatalogClient::kMajorityWriteConcern));

    // Super-inefficient due to limitation of the catalogClient (no multi-document update), but just
    // temporary: TODO on SERVER-53105 completion, throw out the following scope
    {
        // Update all config.chunks entries for the given collection
        repl::OpTime opTime;
        auto chunks = uassertStatusOK(Grid::get(opCtx)->catalogClient()->getChunks(
            opCtx,
            BSON(ChunkType::ns(fromNss.ns())),
            BSON(ChunkType::lastmod() << 1),
            boost::none,
            &opTime,
            repl::ReadConcernLevel::kMajorityReadConcern));


        for (auto& chunk : chunks) {
            uassertStatusOK(
                catalogClient->updateConfigDocument(opCtx,
                                                    ChunkType::ConfigNS,
                                                    BSON(ChunkType::name(chunk.getName())),
                                                    BSON("$set" << BSON(ChunkType::ns(toNss.ns()))),
                                                    false, /* upsert */
                                                    ShardingCatalogClient::kMajorityWriteConcern));
        }
    }
}

void checkShardedRenamePreconditions(OperationContext* opCtx,
                                     const NamespaceString& toNss,
                                     const bool dropTarget) {
    if (!dropTarget) {
        // Check that the sharded target collection doesn't exist
        auto catalogCache = Grid::get(opCtx)->catalogCache();
        try {
            catalogCache->getShardedCollectionRoutingInfo(opCtx, toNss);
            // If no exception is thrown, the collection exists and is sharded
            uasserted(ErrorCodes::CommandFailed,
                      str::stream() << "Sharded target collection " << toNss.ns()
                                    << " exists but dropTarget is not set");
        } catch (const DBException& ex) {
            auto code = ex.code();
            if (code != ErrorCodes::NamespaceNotFound && code != ErrorCodes::NamespaceNotSharded) {
                throw;
            }
        }

        // Check that the unsharded target collection doesn't exist
        auto collectionCatalog = CollectionCatalog::get(opCtx);
        auto targetColl = collectionCatalog->lookupCollectionByNamespace(opCtx, toNss);
        uassert(ErrorCodes::CommandFailed,
                str::stream() << "Target collection " << toNss.ns()
                              << " exists but dropTarget is not set",
                !targetColl);
    }

    // Check that there are no tags associated to the target collection
    auto catalogClient = Grid::get(opCtx)->catalogClient();
    auto tags = uassertStatusOK(catalogClient->getTagsForCollection(opCtx, toNss));
    uassert(ErrorCodes::CommandFailed,
            str::stream() << "Can't rename to target collection " << toNss.ns()
                          << " because it must not have associated tags",
            tags.empty());
}

}  // namespace sharding_ddl_util
}  // namespace mongo

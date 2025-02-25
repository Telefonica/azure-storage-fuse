#pragma once

#include <string>

#include "storage_EXPORTS.h"

#include "http_base.h"
#include "storage_account.h"
#include "storage_request_base.h"

namespace microsoft_azure {
    namespace storage {

        class get_block_list_request_base : public blob_request_base {
        public:
            enum class blocklisttypes {
                committed,
                uncommitted,
                all
            };

            virtual std::string container() const = 0;
            virtual std::string blob() const = 0;

            virtual std::string snapshot() const { return std::string(); }
            virtual blocklisttypes blocklisttype() const { return blocklisttypes::all; }

            AZURE_STORAGE_API void build_request(const storage_account &a, http_base &h, const storage_client_key &k) const override;
        };

        //AZURE_STORAGE_API void build_request(const storage_account &a, const get_blob_request_base &r, http_base &h);

        class get_block_list_item {
        public:
            std::string name;
            unsigned long long size;
        };

        class get_block_list_response {
        public:
            std::vector<get_block_list_item> committed;
            std::vector<get_block_list_item> uncommitted;
        };

    }
}

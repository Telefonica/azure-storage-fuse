#pragma once

#include <chrono>
#include <future>
#include <iterator>
#include <sstream>
#include <thread>

#include "storage_EXPORTS.h"

#include "common.h"
#include "storage_outcome.h"
#include "storage_account.h"
#include "http_base.h"
#include "xml_parser_base.h"
#include "retry.h"
#include "utility.h"

#define HTTP_CODE_SERVICE_UNAVAILABLE 503 //Service unavailable
namespace microsoft_azure {
    namespace storage {
        const char * const xml_parser_ex_literal = "Attempt at parsing XML response failed.";
        class executor_context {
        public:
            executor_context(std::shared_ptr<xml_parser_base> xml_parser, std::shared_ptr<retry_policy_base> retry)
                : m_xml_parser(xml_parser),
                m_retry_policy(retry) {}

            std::shared_ptr<xml_parser_base> xml_parser() const {
                return m_xml_parser;
            }

            std::shared_ptr<retry_policy_base> retry_policy() const {
                return m_retry_policy;
            }

        private:
            std::shared_ptr<xml_parser_base> m_xml_parser;
            std::shared_ptr<retry_policy_base> m_retry_policy;
        };

        template<typename RESPONSE_TYPE>
        class async_executor {
        public:
            static void submit_request(std::promise<storage_outcome<RESPONSE_TYPE>> &promise, const storage_account &a, const storage_request_base &r, const storage_client_key &k, http_base &h, const executor_context &context, retry_context &retry) {
                h.set_error_stream([](http_base::http_code) { return true; }, storage_iostream::create_storage_stream());
                r.build_request(a, h, k);

                retry_info info = context.retry_policy()->evaluate(retry);
                if (info.should_retry()) {
                    h.submit([&promise, &a, &r, &k, &h, &context, &retry](http_base::http_code result, storage_istream s, CURLcode code) {
                        std::string str(std::istreambuf_iterator<char>(s.istream()), std::istreambuf_iterator<char>());
                        if (code != CURLE_OK || unsuccessful(result)) {
                            promise.set_value(storage_outcome<RESPONSE_TYPE>(context.xml_parser()->parse_storage_error(str)));
                            retry.add_result(code == CURLE_OK ? result : HTTP_CODE_SERVICE_UNAVAILABLE);
                            h.reset_input_stream();
                            h.reset_output_stream();
                            async_executor<RESPONSE_TYPE>::submit_request(promise, a, r, k, h, context, retry);
                        }
                        else {
                            promise.set_value(storage_outcome<RESPONSE_TYPE>(context.xml_parser()->parse_response<RESPONSE_TYPE>(str)));
                        }
                    }, info.interval());
                }
            }

            static void submit_helper(
                std::shared_ptr<std::promise<storage_outcome<RESPONSE_TYPE>>> promise,
                std::shared_ptr<storage_outcome<RESPONSE_TYPE>> outcome,
                std::shared_ptr<storage_account> account,
                std::shared_ptr<storage_request_base> request,
                std::shared_ptr<storage_client_key> client_key,
                std::shared_ptr<http_base> http,
                std::shared_ptr<executor_context> context,
                std::shared_ptr<retry_context> retry)
            {
                http->set_error_stream([](http_base::http_code) { return true; }, storage_iostream::create_storage_stream());
                request->build_request(*account, *http, *client_key);
                retry_info info = context->retry_policy()->evaluate(*retry);
                if (info.should_retry())
                {
                    http->submit([promise, outcome, account, request, client_key, http, context, retry](http_base::http_code result, storage_istream s, CURLcode code)
                    {
                        bool retry_response = false;
                        std::string str(std::istreambuf_iterator<char>(s.istream()), std::istreambuf_iterator<char>());
                        if (code != CURLE_OK || unsuccessful(result))
                        {
                            //if we are unable to parse or did not find the values we needed to parse the error response
                            storage_error error;
                            try
                            {
                                error = context->xml_parser()->parse_storage_error(str);
                            }
                            catch(std::invalid_argument& parser_error_except)
                            {
                                int sizeArray = strlen(xml_parser_ex_literal) + sizeof(" ") + strlen(parser_error_except.what());
                                char * cstr_parser_error = (char *)malloc(sizeArray);
                                strcpy(cstr_parser_error, xml_parser_ex_literal);
                                strcat(cstr_parser_error, " ");
                                strcat(cstr_parser_error, parser_error_except.what());
                                syslog(LOG_ERR, "%s", cstr_parser_error);
                            }
                            catch(...)
                            {
                                syslog(LOG_ERR, "%s",xml_parser_ex_literal);
                            }
                            retry_response = true;
                            //to ensure the most helpful error code is returned, if the curl code returns ok
                            //return the http error code
                            error.code = std::to_string(code == CURLE_OK ? result : code);
                            *outcome = storage_outcome<RESPONSE_TYPE>(error);
                            retry->add_result(code == CURLE_OK ? result: HTTP_CODE_SERVICE_UNAVAILABLE);
                        }
                        else
                        {
                            //if we are unable to parse or did not find the values we needed to parse the response
                            //something is corrupt in the response and we need to retry for a better response
                            try
                            {
                                *outcome = storage_outcome<RESPONSE_TYPE>(context->xml_parser()->parse_response<RESPONSE_TYPE>(str));
                            }
                            catch(std::invalid_argument& parser_error_except)
                            {
                                retry_response = true;
                                int sizeArray = strlen(xml_parser_ex_literal) + sizeof(" ") + strlen(parser_error_except.what());
                                char * cstr_parser_error = (char *)malloc(sizeArray);
                                strcpy(cstr_parser_error, xml_parser_ex_literal);
                                strcat(cstr_parser_error, " ");
                                strcat(cstr_parser_error, parser_error_except.what());
                                syslog(LOG_ERR, "%s", cstr_parser_error);
                            }
                            catch(...)
                            {
                                retry_response = true;
                                syslog(LOG_ERR, "%s", xml_parser_ex_literal);
                            }
                            if(!retry_response)
                            {
                                promise->set_value(*outcome);
                            }
                        }
                        //if we receive an error response or a parser error then retry the request for a better response
                        if(retry_response)
                        {
                            http->reset_input_stream();
                            http->reset_output_stream();
                            async_executor<RESPONSE_TYPE>::submit_helper(promise, outcome, account, request, client_key, http, context, retry);
                        }
                    }, info.interval());
                }
                else
                {
                    promise->set_value(*outcome);
                }
            }

            static std::future<storage_outcome<RESPONSE_TYPE>> submit(
                std::shared_ptr<storage_account> account,
                std::shared_ptr<storage_request_base> request,
                std::shared_ptr<storage_client_key> client_key,
                std::shared_ptr<http_base> http,
                std::shared_ptr<executor_context> context)
            {
                auto retry = std::make_shared<retry_context>();
                auto outcome = std::make_shared<storage_outcome<RESPONSE_TYPE>>();
                auto promise = std::make_shared<std::promise<storage_outcome<RESPONSE_TYPE>>>();
                async_executor<RESPONSE_TYPE>::submit_helper(promise, outcome, account, request, client_key, http, context, retry);
                return promise->get_future();
            }
        };

        template<>
        class async_executor<void> {
        public:
            static void submit_request(std::promise<storage_outcome<void>> &promise, const storage_account &a, const storage_request_base &r, const storage_client_key &k, http_base &h, const executor_context &context, retry_context &retry) {
                h.set_error_stream(unsuccessful, storage_iostream::create_storage_stream());
                r.build_request(a, h, k);

                retry_info info = context.retry_policy()->evaluate(retry);
                if (info.should_retry()) {
                    h.submit([&promise, &a, &r, &k, &h, &context, &retry](http_base::http_code result, storage_istream s, CURLcode code) {
                        std::string str(std::istreambuf_iterator<char>(s.istream()), std::istreambuf_iterator<char>());
                        if (code != CURLE_OK || unsuccessful(result)) {
                            try
                            {
                                promise.set_value(storage_outcome<void>(context.xml_parser()->parse_storage_error(str)));
                            }
                            catch(std::invalid_argument & parser_error_except)
                            {
                                int sizeArray = strlen(xml_parser_ex_literal) + sizeof(" ") + strlen(parser_error_except.what());
                                char * cstr_parser_error = (char *)malloc(sizeArray);
                                strcpy(cstr_parser_error, xml_parser_ex_literal);
                                strcat(cstr_parser_error, " ");
                                strcat(cstr_parser_error, parser_error_except.what());
                                syslog(LOG_ERR, "%s", cstr_parser_error);
                            }
                            catch(...)
                            {
                                syslog(LOG_ERR,"%s", xml_parser_ex_literal);
                            }
                            retry.add_result(code == CURLE_OK ? result : HTTP_CODE_SERVICE_UNAVAILABLE);
                            h.reset_input_stream();
                            h.reset_output_stream();
                            async_executor<void>::submit_request(promise, a, r, k, h, context, retry);
                        }
                        else {
                            promise.set_value(storage_outcome<void>());
                        }
                    }, info.interval());
                }
            }

            static void submit_helper(
                std::shared_ptr<std::promise<storage_outcome<void>>> promise,
                std::shared_ptr<storage_outcome<void>> outcome,
                std::shared_ptr<storage_account> account,
                std::shared_ptr<storage_request_base> request,
                std::shared_ptr<storage_client_key> client_key,
                std::shared_ptr<http_base> http,
                std::shared_ptr<executor_context> context,
                std::shared_ptr<retry_context> retry)
            {
                http->reset();
                http->set_error_stream(unsuccessful, storage_iostream::create_storage_stream());
                request->build_request(*account, *http, *client_key);

                retry_info info = context->retry_policy()->evaluate(*retry);
                if (info.should_retry())
                {
                    http->submit([promise, outcome, account, request, client_key, http, context, retry](http_base::http_code result, storage_istream s, CURLcode code)
                    {
                        std::string str(std::istreambuf_iterator<char>(s.istream()), std::istreambuf_iterator<char>());
                        if (code != CURLE_OK || unsuccessful(result))
                        {
                            storage_error error;
                            try
                            {
                                error = context->xml_parser()->parse_storage_error(str);
                            }
                            catch(std::invalid_argument & parser_error_except)
                            {
                                int sizeArray = strlen(xml_parser_ex_literal) + sizeof(" ") + strlen(parser_error_except.what());
                                char * cstr_parser_error = (char *)malloc(sizeArray);
                                strcpy(cstr_parser_error, xml_parser_ex_literal);
                                strcat(cstr_parser_error, " ");
                                strcat(cstr_parser_error, parser_error_except.what());
                                syslog(LOG_ERR, "%s", cstr_parser_error);
                            }
                            catch(...)
                            {
                                syslog(LOG_ERR,"%s", xml_parser_ex_literal);
                            }
                            //to ensure the most helpful error code is returned, if the curl code returns ok
                            //return the http error code
                            error.code = std::to_string(code == CURLE_OK ? result : code);
                            *outcome = storage_outcome<void>(error);
                            retry->add_result(code == CURLE_OK ? result: HTTP_CODE_SERVICE_UNAVAILABLE);
                            http->reset_input_stream();
                            http->reset_output_stream();
                            async_executor<void>::submit_helper(promise, outcome, account, request, client_key, http, context, retry);
                        }
                        else
                        {
                            *outcome = storage_outcome<void>();
                            promise->set_value(*outcome);
                        }
                    }, info.interval());
                }
                else
                {
                    promise->set_value(*outcome);
                }
            }

            static std::future<storage_outcome<void>> submit(
                std::shared_ptr<storage_account> account,
                std::shared_ptr<storage_request_base> request,
                std::shared_ptr<storage_client_key> client_key,
                std::shared_ptr<http_base> http,
                std::shared_ptr<executor_context> context)
            {
                auto retry = std::make_shared<retry_context>();
                auto outcome = std::make_shared<storage_outcome<void>>();
                auto promise = std::make_shared<std::promise<storage_outcome<void>>>();
                async_executor<void>::submit_helper(promise, outcome, account, request, client_key, http, context, retry);
                return promise->get_future();
            }
        };
    }
}


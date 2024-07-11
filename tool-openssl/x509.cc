// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0 OR ISC

#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <cstdio>
#include <ctime>
#include "internal.h"

static const argument_t kArguments[] = {
    { "-in", kRequiredArgument, "Input file" },
    { "-out", kOptionalArgument, "Output file" },
    { "-noout", kBooleanArgument, "No output" },
    { "-modulus", kBooleanArgument, "Modulus" },
    { "-signkey", kOptionalArgument, "Sign key" },
    { "-days", kOptionalArgument, "Days" },
    { "-dates", kBooleanArgument, "Dates" },
    { "-req", kBooleanArgument, "Req" },
    { "-checkend", kOptionalArgument, "Checkend" },
    { "", kOptionalArgument, "" }
};

bool WriteSignedCertificate(X509 *x509, const std::string &out_path) {
  ScopedFILE out_file(fopen(out_path.c_str(), "wb"));
  if (!out_file) {
    fprintf(stderr, "Error: unable to open output file '%s'\n", out_path.c_str());
    return false;
  }
  if (!PEM_write_X509(out_file.get(), x509)) {
    fprintf(stderr, "Error: error writing certificate to '%s'\n", out_path.c_str());
    ERR_print_errors_fp(stderr);
    return false;
  }
  return true;
}

bool LoadAndSignCertificate(X509 *x509, const std::string &signkey_path) {
  ScopedFILE signkey_file(fopen(signkey_path.c_str(), "rb"));
  if (!signkey_file) {
    fprintf(stderr, "Error: unable to load private key from '%s'\n", signkey_path.c_str());
    return false;
  }
  EVP_PKEY *pkey = PEM_read_PrivateKey(signkey_file.get(), nullptr, nullptr, nullptr);
  if (!pkey) {
    fprintf(stderr, "Error: error reading private key from '%s'\n", signkey_path.c_str());
    ERR_print_errors_fp(stderr);
    return false;
  }
  bssl::UniquePtr<EVP_PKEY> pkey_guard(pkey);
  if (!X509_sign(x509, pkey, EVP_sha256())) {
    fprintf(stderr, "Error: error signing certificate with key from '%s'\n", signkey_path.c_str());
    ERR_print_errors_fp(stderr);
    return false;
  }
  return true;
}

// Map arguments using tool/args.cc
bool X509Tool(const args_list_t &args) {
  args_map_t parsed_args;
  if (!ParseKeyValueArguments(&parsed_args, args, kArguments)) {
    PrintUsage(kArguments);
    return false;
  }

  std::string in_path, out_path, signkey_path;
  bool noout = false, modulus = false, dates = false, req = false;
  int checkend = 0, days = 0;

  GetString(&in_path, "-in", "", parsed_args);
  GetString(&out_path, "-out", "", parsed_args);
  GetString(&signkey_path, "-signkey", "", parsed_args);
  GetBoolArgument(&noout, "-noout", parsed_args);
  GetBoolArgument(&modulus, "-modulus", parsed_args);
  GetBoolArgument(&dates, "-dates", parsed_args);
  GetBoolArgument(&req, "-req", parsed_args);
  GetUnsigned(reinterpret_cast<unsigned*>(&checkend), "-checkend", 0, parsed_args);
  GetUnsigned(reinterpret_cast<unsigned*>(&days), "-days", 0, parsed_args);

  // Check for required option -in, and -req must include -signkey
  if (in_path.empty()) {
    fprintf(stderr, "Error: missing required argument '-in'\n");
    PrintUsage(kArguments);
    return false;\
  }
  if (req && signkey_path.empty()) {
    fprintf(stderr, "Error: '-req' option must be used with '-signkey' option\n");
    return false;
  }

  // Check for mutually exclusive options
  if (noout && (!out_path.empty() || modulus || dates || checkend)) {
    fprintf(stderr, "Error: '-noout' option cannot be used with '-out', '-modulus', '-dates', and '-checkend' options\n");
    return false;
  }
  if (req && (dates || checkend)){
    fprintf(stderr, "Error: '-req' option cannot be used with '-dates' and '-checkend' options\n");
    return false;
  }
  if (!signkey_path.empty() && (dates || checkend)){
    fprintf(stderr, "Error: '-signkey' option cannot be used with '-dates' and '-checkend' options\n");
    return false;
  }
  if (days && (dates || checkend)){
    fprintf(stderr, "Error: '-days' option cannot be used with '-dates' and '-checkend' options\n");
    return false;
  }

  ScopedFILE in_file(fopen(in_path.c_str(), "rb"));
  if (!in_file) {
    fprintf(stderr, "Error: unable to load certificate from '%s'\n", in_path.c_str());
    return false;
  }

  if (req) {
    bssl::UniquePtr<X509_REQ> csr(PEM_read_X509_REQ(in_file.get(), nullptr, nullptr, nullptr));
    if (!csr) {
      fprintf(stderr, "Error: error parsing CSR from '%s'\n", in_path.c_str());
      ERR_print_errors_fp(stderr);
      return false;
    }

    // Create and sign certificate based on CSR
    bssl::UniquePtr<X509> x509(X509_new());
    if (!x509) {
      fprintf(stderr, "Error: unable to create new X509 certificate\n");
      return false;
    }

    // Set the subject from CSR
    if (!X509_set_subject_name(x509.get(), X509_REQ_get_subject_name(csr.get()))) {
      fprintf(stderr, "Error: unable to set subject name from CSR\n");
      return false;
    }

    // Set the public key from CSR
    bssl::UniquePtr<EVP_PKEY> csr_pkey(X509_REQ_get_pubkey(csr.get()));
    if (!csr_pkey || !X509_set_pubkey(x509.get(), csr_pkey.get())) {
      fprintf(stderr, "Error: unable to set public key from CSR\n");
      return false;
    }

    // Set issuer name
    if (!X509_set_issuer_name(x509.get(), X509_REQ_get_subject_name(csr.get()))) {
      fprintf(stderr, "Error: unable to set issuer name\n");
      return false;
    }

    // Set validity period, default 30 days if not specified
    int valid_days = days > 0 ? days : 30;
    if (!X509_gmtime_adj(X509_getm_notBefore(x509.get()), 0) ||
      !X509_gmtime_adj(X509_getm_notAfter(x509.get()), 60 * 60 * 24 * static_cast<long>(valid_days))) {
      fprintf(stderr, "Error: unable to set validity period\n");
      return false;
    }

    // Sign the certificate with the provided key
    if (!signkey_path.empty()) {
      if (!LoadAndSignCertificate(x509.get(), signkey_path)) {
        return false;
      }
    }

    // Write the signed certificate to output file
    if (!noout && !out_path.empty()) {
      if (!WriteSignedCertificate(x509.get(), out_path)) {
        return false;
      }
    }
  } else {
    // Parse x509 certificate from input PEM file
    bssl::UniquePtr<X509> x509(PEM_read_X509(in_file.get(), nullptr, nullptr, nullptr));
    if (!x509) {
      fprintf(stderr, "Error: error parsing certificate from '%s'\n", in_path.c_str());
      ERR_print_errors_fp(stderr);
      return false;
    }

    if (dates) {
      BIO *bio = BIO_new(BIO_s_mem());

      if (ASN1_TIME_print(bio, X509_get_notBefore(x509.get()))) {
        char not_before[30] = {};
        BIO_read(bio, not_before, sizeof(not_before) - 1);
        fprintf(stdout, "notBefore=%s\n", not_before);
      }

      if (ASN1_TIME_print(bio, X509_get_notAfter(x509.get()))) {
        char not_after[30] = {};
        BIO_read(bio, not_after, sizeof(not_after) - 1);
        fprintf(stdout, "notAfter=%s\n", not_after);
      }
      BIO_free(bio);
    }

    if (modulus) {
      bssl::UniquePtr<EVP_PKEY> pkey(X509_get_pubkey(x509.get()));
      if (!pkey) {
        fprintf(stderr, "Error: unable to load public key from certificate\n");
        return false;
      }

      if (EVP_PKEY_base_id(pkey.get()) == EVP_PKEY_RSA) {
        const RSA *rsa = EVP_PKEY_get0_RSA(pkey.get());
        if (!rsa) {
          fprintf(stderr, "Error: unable to load RSA key\n");
          return false;
        }
        const BIGNUM *n = RSA_get0_n(rsa);
        if (!n) {
          fprintf(stderr, "Error: unable to load modulus\n");
          return false;
        }
        printf("Modulus=");
        BN_print_fp(stdout, n);
        printf("\n");
      } else {
        fprintf(stderr, "Error: public key is not an RSA key\n");
      }
    }

    if (checkend) {
      ASN1_TIME *current_time = ASN1_TIME_set(nullptr, std::time(nullptr));
      ASN1_TIME *end_time = X509_getm_notAfter(x509.get());
      int days_left, seconds_left;
      if (!ASN1_TIME_diff(&days_left, &seconds_left, current_time, end_time)) {
        fprintf(stderr, "Error: failed to calculate time difference\n");
        ASN1_TIME_free(current_time);
        return false;
      }
      ASN1_TIME_free(current_time);

      if ((days_left * 86400 + seconds_left) < checkend) {
        printf("Certificate will expire\n");
      } else {
        printf("Certificate will not expire\n");
      }
    }

    if (!signkey_path.empty()) {
      if (!LoadAndSignCertificate(x509.get(), signkey_path)) {
        return false;
      }
    }

    if (!noout && !out_path.empty()) {
      if (!WriteSignedCertificate(x509.get(), out_path)) {
        return false;
      }
    }
  }
  return true;
}

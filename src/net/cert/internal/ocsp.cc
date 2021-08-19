// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/cert/internal/ocsp.h"

#include <algorithm>

#include "base/base64.h"
#include "base/strings/string_util.h"
#include "base/time/time.h"
#include "net/cert/asn1_util.h"
#include "net/cert/internal/cert_errors.h"
#include "net/cert/internal/extended_key_usage.h"
#include "net/cert/internal/parsed_certificate.h"
#include "net/cert/internal/verify_name_match.h"
#include "net/cert/internal/verify_signed_data.h"
#include "net/cert/x509_util.h"
#include "net/der/encode_values.h"
#include "third_party/boringssl/src/include/openssl/bytestring.h"
#include "third_party/boringssl/src/include/openssl/digest.h"
#include "third_party/boringssl/src/include/openssl/mem.h"
#include "third_party/boringssl/src/include/openssl/sha.h"
#include "url/gurl.h"

namespace net {

OCSPCertID::OCSPCertID() {}
OCSPCertID::~OCSPCertID() {}

OCSPSingleResponse::OCSPSingleResponse() {}
OCSPSingleResponse::~OCSPSingleResponse() {}

OCSPResponseData::OCSPResponseData() {}
OCSPResponseData::~OCSPResponseData() {}

OCSPResponse::OCSPResponse() {}
OCSPResponse::~OCSPResponse() {}

der::Input BasicOCSPResponseOid() {
  // From RFC 6960:
  //
  // id-pkix-ocsp           OBJECT IDENTIFIER ::= { id-ad-ocsp }
  // id-pkix-ocsp-basic     OBJECT IDENTIFIER ::= { id-pkix-ocsp 1 }
  //
  // In dotted notation: 1.3.6.1.5.5.7.48.1.1
  static const uint8_t oid[] = {0x2b, 0x06, 0x01, 0x05, 0x05,
                                0x07, 0x30, 0x01, 0x01};
  return der::Input(oid);
}

// CertID ::= SEQUENCE {
//    hashAlgorithm           AlgorithmIdentifier,
//    issuerNameHash          OCTET STRING, -- Hash of issuer's DN
//    issuerKeyHash           OCTET STRING, -- Hash of issuer's public key
//    serialNumber            CertificateSerialNumber
// }
bool ParseOCSPCertID(const der::Input& raw_tlv, OCSPCertID* out) {
  der::Parser outer_parser(raw_tlv);
  der::Parser parser;
  if (!outer_parser.ReadSequence(&parser))
    return false;
  if (outer_parser.HasMore())
    return false;

  der::Input sigalg_tlv;
  if (!parser.ReadRawTLV(&sigalg_tlv))
    return false;
  if (!ParseHashAlgorithm(sigalg_tlv, &(out->hash_algorithm)))
    return false;
  if (!parser.ReadTag(der::kOctetString, &(out->issuer_name_hash)))
    return false;
  if (!parser.ReadTag(der::kOctetString, &(out->issuer_key_hash)))
    return false;
  if (!parser.ReadTag(der::kInteger, &(out->serial_number)))
    return false;
  CertErrors errors;
  if (!VerifySerialNumber(out->serial_number, false /*warnings_only*/, &errors))
    return false;

  return !parser.HasMore();
}

namespace {

// Parses |raw_tlv| to extract an OCSP RevokedInfo (RFC 6960) and stores the
// result in the OCSPCertStatus |out|. Returns whether the parsing was
// successful.
//
// RevokedInfo ::= SEQUENCE {
//      revocationTime              GeneralizedTime,
//      revocationReason    [0]     EXPLICIT CRLReason OPTIONAL
// }
bool ParseRevokedInfo(const der::Input& raw_tlv, OCSPCertStatus* out) {
  der::Parser parser(raw_tlv);
  if (!parser.ReadGeneralizedTime(&(out->revocation_time)))
    return false;

  der::Input reason_input;
  if (!parser.ReadOptionalTag(der::ContextSpecificConstructed(0), &reason_input,
                              &(out->has_reason))) {
    return false;
  }
  if (out->has_reason) {
    der::Parser reason_parser(reason_input);
    der::Input reason_value_input;
    uint8_t reason_value;
    if (!reason_parser.ReadTag(der::kEnumerated, &reason_value_input))
      return false;
    if (!der::ParseUint8(reason_value_input, &reason_value))
      return false;
    if (reason_value >
        static_cast<uint8_t>(OCSPCertStatus::RevocationReason::LAST)) {
      return false;
    }
    out->revocation_reason =
        static_cast<OCSPCertStatus::RevocationReason>(reason_value);
    if (out->revocation_reason == OCSPCertStatus::RevocationReason::UNUSED)
      return false;
    if (reason_parser.HasMore())
      return false;
  }
  return !parser.HasMore();
}

// Parses |raw_tlv| to extract an OCSP CertStatus (RFC 6960) and stores the
// result in the OCSPCertStatus |out|. Returns whether the parsing was
// successful.
//
// CertStatus ::= CHOICE {
//      good        [0]     IMPLICIT NULL,
//      revoked     [1]     IMPLICIT RevokedInfo,
//      unknown     [2]     IMPLICIT UnknownInfo
// }
//
// UnknownInfo ::= NULL
bool ParseCertStatus(const der::Input& raw_tlv, OCSPCertStatus* out) {
  der::Parser parser(raw_tlv);
  der::Tag status_tag;
  der::Input status;
  if (!parser.ReadTagAndValue(&status_tag, &status))
    return false;

  out->has_reason = false;
  if (status_tag == der::ContextSpecificPrimitive(0)) {
    out->status = OCSPRevocationStatus::GOOD;
  } else if (status_tag == der::ContextSpecificConstructed(1)) {
    out->status = OCSPRevocationStatus::REVOKED;
    if (!ParseRevokedInfo(status, out))
      return false;
  } else if (status_tag == der::ContextSpecificPrimitive(2)) {
    out->status = OCSPRevocationStatus::UNKNOWN;
  } else {
    return false;
  }

  return !parser.HasMore();
}

// Writes the hash of |value| as an OCTET STRING to |cbb|, using |hash_type| as
// the algorithm. Returns true on success.
bool AppendHashAsOctetString(const EVP_MD* hash_type,
                             CBB* cbb,
                             const der::Input& value) {
  CBB octet_string;
  unsigned hash_len;
  uint8_t hash_buffer[EVP_MAX_MD_SIZE];

  return CBB_add_asn1(cbb, &octet_string, CBS_ASN1_OCTETSTRING) &&
         EVP_Digest(value.UnsafeData(), value.Length(), hash_buffer, &hash_len,
                    hash_type, nullptr) &&
         CBB_add_bytes(&octet_string, hash_buffer, hash_len) && CBB_flush(cbb);
}

}  // namespace

// SingleResponse ::= SEQUENCE {
//      certID                       CertID,
//      certStatus                   CertStatus,
//      thisUpdate                   GeneralizedTime,
//      nextUpdate         [0]       EXPLICIT GeneralizedTime OPTIONAL,
//      singleExtensions   [1]       EXPLICIT Extensions OPTIONAL
// }
bool ParseOCSPSingleResponse(const der::Input& raw_tlv,
                             OCSPSingleResponse* out) {
  der::Parser outer_parser(raw_tlv);
  der::Parser parser;
  if (!outer_parser.ReadSequence(&parser))
    return false;
  if (outer_parser.HasMore())
    return false;

  if (!parser.ReadRawTLV(&(out->cert_id_tlv)))
    return false;
  der::Input status_tlv;
  if (!parser.ReadRawTLV(&status_tlv))
    return false;
  if (!ParseCertStatus(status_tlv, &(out->cert_status)))
    return false;
  if (!parser.ReadGeneralizedTime(&(out->this_update)))
    return false;

  der::Input next_update_input;
  if (!parser.ReadOptionalTag(der::ContextSpecificConstructed(0),
                              &next_update_input, &(out->has_next_update))) {
    return false;
  }
  if (out->has_next_update) {
    der::Parser next_update_parser(next_update_input);
    if (!next_update_parser.ReadGeneralizedTime(&(out->next_update)))
      return false;
    if (next_update_parser.HasMore())
      return false;
  }

  if (!parser.ReadOptionalTag(der::ContextSpecificConstructed(1),
                              &(out->extensions), &(out->has_extensions))) {
    return false;
  }

  return !parser.HasMore();
}

namespace {

// Parses |raw_tlv| to extract a ResponderID (RFC 6960) and stores the
// result in the ResponderID |out|. Returns whether the parsing was successful.
//
// ResponderID ::= CHOICE {
//      byName               [1] Name,
//      byKey                [2] KeyHash
// }
bool ParseResponderID(const der::Input& raw_tlv,
                      OCSPResponseData::ResponderID* out) {
  der::Parser parser(raw_tlv);
  der::Tag id_tag;
  der::Input id_input;
  if (!parser.ReadTagAndValue(&id_tag, &id_input))
    return false;

  if (id_tag == der::ContextSpecificConstructed(1)) {
    out->type = OCSPResponseData::ResponderType::NAME;
    out->name = id_input;
  } else if (id_tag == der::ContextSpecificConstructed(2)) {
    der::Parser key_parser(id_input);
    der::Input key_hash;
    if (!key_parser.ReadTag(der::kOctetString, &key_hash))
      return false;
    if (key_parser.HasMore())
      return false;
    if (key_hash.Length() != SHA_DIGEST_LENGTH)
      return false;

    out->type = OCSPResponseData::ResponderType::KEY_HASH;
    out->key_hash = key_hash;
  } else {
    return false;
  }
  return !parser.HasMore();
}

}  // namespace

// ResponseData ::= SEQUENCE {
//      version              [0] EXPLICIT Version DEFAULT v1,
//      responderID              ResponderID,
//      producedAt               GeneralizedTime,
//      responses                SEQUENCE OF SingleResponse,
//      responseExtensions   [1] EXPLICIT Extensions OPTIONAL
// }
bool ParseOCSPResponseData(const der::Input& raw_tlv, OCSPResponseData* out) {
  der::Parser outer_parser(raw_tlv);
  der::Parser parser;
  if (!outer_parser.ReadSequence(&parser))
    return false;
  if (outer_parser.HasMore())
    return false;

  der::Input version_input;
  bool version_present;
  if (!parser.ReadOptionalTag(der::ContextSpecificConstructed(0),
                              &version_input, &version_present)) {
    return false;
  }

  // For compatibilty, we ignore the restriction from X.690 Section 11.5 that
  // DEFAULT values should be omitted for values equal to the default value.
  // TODO: Add warning about non-strict parsing.
  if (version_present) {
    der::Parser version_parser(version_input);
    if (!version_parser.ReadUint8(&(out->version)))
      return false;
    if (version_parser.HasMore())
      return false;
  } else {
    out->version = 0;
  }

  if (out->version != 0)
    return false;

  der::Input responder_input;
  if (!parser.ReadRawTLV(&responder_input))
    return false;
  if (!ParseResponderID(responder_input, &(out->responder_id)))
    return false;
  if (!parser.ReadGeneralizedTime(&(out->produced_at)))
    return false;

  der::Parser responses_parser;
  if (!parser.ReadSequence(&responses_parser))
    return false;
  out->responses.clear();
  while (responses_parser.HasMore()) {
    der::Input single_response;
    if (!responses_parser.ReadRawTLV(&single_response))
      return false;
    out->responses.push_back(single_response);
  }

  if (!parser.ReadOptionalTag(der::ContextSpecificConstructed(1),
                              &(out->extensions), &(out->has_extensions))) {
    return false;
  }

  return !parser.HasMore();
}

namespace {

// Parses |raw_tlv| to extract a BasicOCSPResponse (RFC 6960) and stores the
// result in the OCSPResponse |out|. Returns whether the parsing was
// successful.
//
// BasicOCSPResponse       ::= SEQUENCE {
//      tbsResponseData      ResponseData,
//      signatureAlgorithm   AlgorithmIdentifier,
//      signature            BIT STRING,
//      certs            [0] EXPLICIT SEQUENCE OF Certificate OPTIONAL
// }
bool ParseBasicOCSPResponse(const der::Input& raw_tlv, OCSPResponse* out) {
  der::Parser outer_parser(raw_tlv);
  der::Parser parser;
  if (!outer_parser.ReadSequence(&parser))
    return false;
  if (outer_parser.HasMore())
    return false;

  if (!parser.ReadRawTLV(&(out->data)))
    return false;
  der::Input sigalg_tlv;
  if (!parser.ReadRawTLV(&sigalg_tlv))
    return false;
  // TODO(crbug.com/634443): Propagate the errors.
  CertErrors errors;
  out->signature_algorithm = SignatureAlgorithm::Create(sigalg_tlv, &errors);
  if (!out->signature_algorithm)
    return false;
  if (!parser.ReadBitString(&(out->signature)))
    return false;
  der::Input certs_input;
  if (!parser.ReadOptionalTag(der::ContextSpecificConstructed(0), &certs_input,
                              &(out->has_certs))) {
    return false;
  }

  out->certs.clear();
  if (out->has_certs) {
    der::Parser certs_seq_parser(certs_input);
    der::Parser certs_parser;
    if (!certs_seq_parser.ReadSequence(&certs_parser))
      return false;
    if (certs_seq_parser.HasMore())
      return false;
    while (certs_parser.HasMore()) {
      der::Input cert_tlv;
      if (!certs_parser.ReadRawTLV(&cert_tlv))
        return false;
      out->certs.push_back(cert_tlv);
    }
  }

  return !parser.HasMore();
}

}  // namespace

// OCSPResponse ::= SEQUENCE {
//      responseStatus         OCSPResponseStatus,
//      responseBytes          [0] EXPLICIT ResponseBytes OPTIONAL
// }
//
// ResponseBytes ::=       SEQUENCE {
//      responseType   OBJECT IDENTIFIER,
//      response       OCTET STRING
// }
bool ParseOCSPResponse(const der::Input& raw_tlv, OCSPResponse* out) {
  der::Parser outer_parser(raw_tlv);
  der::Parser parser;
  if (!outer_parser.ReadSequence(&parser))
    return false;
  if (outer_parser.HasMore())
    return false;

  der::Input response_status_input;
  uint8_t response_status;
  if (!parser.ReadTag(der::kEnumerated, &response_status_input))
    return false;
  if (!der::ParseUint8(response_status_input, &response_status))
    return false;
  if (response_status >
      static_cast<uint8_t>(OCSPResponse::ResponseStatus::LAST)) {
    return false;
  }
  out->status = static_cast<OCSPResponse::ResponseStatus>(response_status);
  if (out->status == OCSPResponse::ResponseStatus::UNUSED)
    return false;

  if (out->status == OCSPResponse::ResponseStatus::SUCCESSFUL) {
    der::Parser outer_bytes_parser;
    der::Parser bytes_parser;
    if (!parser.ReadConstructed(der::ContextSpecificConstructed(0),
                                &outer_bytes_parser)) {
      return false;
    }
    if (!outer_bytes_parser.ReadSequence(&bytes_parser))
      return false;
    if (outer_bytes_parser.HasMore())
      return false;

    der::Input type_oid;
    if (!bytes_parser.ReadTag(der::kOid, &type_oid))
      return false;
    if (type_oid != BasicOCSPResponseOid())
      return false;

    // As per RFC 6960 Section 4.2.1, the value of |response| SHALL be the DER
    // encoding of BasicOCSPResponse.
    der::Input response;
    if (!bytes_parser.ReadTag(der::kOctetString, &response))
      return false;
    if (!ParseBasicOCSPResponse(response, out))
      return false;
    if (bytes_parser.HasMore())
      return false;
  }

  return !parser.HasMore();
}

namespace {

// Checks that the |type| hash of |value| is equal to |hash|
bool VerifyHash(const EVP_MD* type,
                const der::Input& hash,
                const der::Input& value) {
  unsigned value_hash_len;
  uint8_t value_hash[EVP_MAX_MD_SIZE];
  if (!EVP_Digest(value.UnsafeData(), value.Length(), value_hash,
                  &value_hash_len, type, nullptr)) {
    return false;
  }

  return hash == der::Input(value_hash, value_hash_len);
}

// Extracts the bytes of the SubjectPublicKey bit string given an SPKI. That is
// to say, the value of subjectPublicKey without the leading unused bit
// count octet.
//
// Returns true on success and fills |*spk_tlv| with the result.
//
// SubjectPublicKeyInfo  ::=  SEQUENCE  {
//     algorithm            AlgorithmIdentifier,
//     subjectPublicKey     BIT STRING
// }
bool GetSubjectPublicKeyBytes(const der::Input& spki_tlv, der::Input* spk_tlv) {
  base::StringPiece spk_strpiece;
  if (!asn1::ExtractSubjectPublicKeyFromSPKI(spki_tlv.AsStringPiece(),
                                             &spk_strpiece)) {
    return false;
  }

  // ExtractSubjectPublicKeyFromSPKI() includes the unused bit count. For this
  // application, the unused bit count must be zero, and is not included in the
  // result.
  if (!spk_strpiece.starts_with("\0"))
    return false;
  spk_strpiece.remove_prefix(1);

  *spk_tlv = der::Input(spk_strpiece);
  return true;
}

// Checks the OCSPCertID |id| identifies |certificate|.
bool CheckCertIDMatchesCertificate(
    const OCSPCertID& id,
    const ParsedCertificate* certificate,
    const ParsedCertificate* issuer_certificate) {
  const EVP_MD* type = nullptr;
  switch (id.hash_algorithm) {
    case DigestAlgorithm::Md2:
    case DigestAlgorithm::Md4:
    case DigestAlgorithm::Md5:
      // Unsupported.
      return false;
    case DigestAlgorithm::Sha1:
      type = EVP_sha1();
      break;
    case DigestAlgorithm::Sha256:
      type = EVP_sha256();
      break;
    case DigestAlgorithm::Sha384:
      type = EVP_sha384();
      break;
    case DigestAlgorithm::Sha512:
      type = EVP_sha512();
      break;
  }

  if (!VerifyHash(type, id.issuer_name_hash, certificate->tbs().issuer_tlv))
    return false;

  der::Input key_tlv;
  if (!GetSubjectPublicKeyBytes(issuer_certificate->tbs().spki_tlv, &key_tlv))
    return false;

  if (!VerifyHash(type, id.issuer_key_hash, key_tlv))
    return false;

  return id.serial_number == certificate->tbs().serial_number;
}

// TODO(eroman): Revisit how certificate parsing is used by this file. Ideally
// would either pass in the parsed bits, or have a better abstraction for lazily
// parsing.
scoped_refptr<ParsedCertificate> ParseCertificate(base::StringPiece der) {
  ParseCertificateOptions parse_options;
  parse_options.allow_invalid_serial_numbers = true;

  // TODO(eroman): Swallows the parsing errors. However uses a permissive
  // parsing model.
  CertErrors errors;
  return ParsedCertificate::Create(
      x509_util::CreateCryptoBuffer(
          reinterpret_cast<const uint8_t*>(der.data()), der.size()),
      {}, &errors);
}

// Checks that the ResponderID |id| matches the certificate |cert| either
// by verifying the name matches that of the certificate or that the hash
// matches the certificate's public key hash (RFC 6960, 4.2.2.3).
WARN_UNUSED_RESULT bool CheckResponderIDMatchesCertificate(
    const OCSPResponseData::ResponderID& id,
    const ParsedCertificate* cert) {
  switch (id.type) {
    case OCSPResponseData::ResponderType::NAME: {
      der::Input name_rdn;
      der::Input cert_rdn;
      if (!der::Parser(id.name).ReadTag(der::kSequence, &name_rdn) ||
          !der::Parser(cert->tbs().subject_tlv)
               .ReadTag(der::kSequence, &cert_rdn))
        return false;
      return VerifyNameMatch(name_rdn, cert_rdn);
    }
    case OCSPResponseData::ResponderType::KEY_HASH: {
      der::Input key;
      if (!GetSubjectPublicKeyBytes(cert->tbs().spki_tlv, &key))
        return false;
      return VerifyHash(EVP_sha1(), id.key_hash, key);
    }
  }

  return false;
}

// Verifies that |responder_certificate| has been authority for OCSP signing,
// delegated to it by |issuer_certificate|.
//
// TODO(eroman): No revocation checks are done (see id-pkix-ocsp-nocheck in the
//     spec). extension).
//
// TODO(eroman): Not all properties of the certificate are verified, only the
//     signature and EKU. Can full RFC 5280 validation be used, or are there
//     compatibility concerns?
WARN_UNUSED_RESULT bool VerifyAuthorizedResponderCert(
    const ParsedCertificate* responder_certificate,
    const ParsedCertificate* issuer_certificate) {
  // The Authorized Responder must be directly signed by the issuer of the
  // certificate being checked.
  // TODO(eroman): Must check the signature algorithm against policy.
  if (!VerifySignedData(responder_certificate->signature_algorithm(),
                        responder_certificate->tbs_certificate_tlv(),
                        responder_certificate->signature_value(),
                        issuer_certificate->tbs().spki_tlv)) {
    return false;
  }

  // The Authorized Responder must include the value id-kp-OCSPSigning as
  // part of the extended key usage extension.
  if (!responder_certificate->has_extended_key_usage())
    return false;
  const std::vector<der::Input>& ekus =
      responder_certificate->extended_key_usage();
  if (std::find(ekus.begin(), ekus.end(), OCSPSigning()) == ekus.end())
    return false;

  return true;
}

WARN_UNUSED_RESULT bool VerifyOCSPResponseSignatureGivenCert(
    const OCSPResponse& response,
    const ParsedCertificate* cert) {
  // TODO(eroman): Must check the signature algorithm against policy.
  return VerifySignedData(*(response.signature_algorithm), response.data,
                          response.signature, cert->tbs().spki_tlv);
}

// Verifies that the OCSP response has a valid signature using
// |issuer_certificate|, or an authorized responder issued by
// |issuer_certificate| for OCSP signing.
WARN_UNUSED_RESULT bool VerifyOCSPResponseSignature(
    const OCSPResponse& response,
    const OCSPResponseData& response_data,
    const ParsedCertificate* issuer_certificate) {
  // In order to verify the OCSP signature, a valid responder matching the OCSP
  // Responder ID must be located (RFC 6960, 4.2.2.2). The responder is allowed
  // to be either the certificate issuer or a delegated authority directly
  // signed by the issuer.
  if (CheckResponderIDMatchesCertificate(response_data.responder_id,
                                         issuer_certificate) &&
      VerifyOCSPResponseSignatureGivenCert(response, issuer_certificate)) {
    return true;
  }

  // Otherwise search through the provided certificates for the Authorized
  // Responder. Want a certificate that:
  //  (1) Matches the OCSP Responder ID.
  //  (2) Has been given authority for OCSP signing by |issuer_certificate|.
  //  (3) Has signed the OCSP response using its public key.
  for (const auto& responder_cert_tlv : response.certs) {
    scoped_refptr<ParsedCertificate> cur_responder_certificate =
        ParseCertificate(responder_cert_tlv.AsStringPiece());

    // If failed parsing the certificate, keep looking.
    if (!cur_responder_certificate)
      continue;

    // If the certificate doesn't match the OCSP's responder ID, keep looking.
    if (!CheckResponderIDMatchesCertificate(response_data.responder_id,
                                            cur_responder_certificate.get())) {
      continue;
    }

    // If the certificate isn't a valid Authorized Responder certificate, keep
    // looking.
    if (!VerifyAuthorizedResponderCert(cur_responder_certificate.get(),
                                       issuer_certificate)) {
      continue;
    }

    // If the certificate signed this OCSP response, have found a match.
    // Otherwise keep looking.
    if (VerifyOCSPResponseSignatureGivenCert(response,
                                             cur_responder_certificate.get())) {
      return true;
    }
  }

  // Failed to confirm the validity of the OCSP signature using any of the
  // candidate certificates.
  return false;
}

// Loops through the OCSPSingleResponses to find the best match for |cert|.
OCSPRevocationStatus GetRevocationStatusForCert(
    const OCSPResponseData& response_data,
    const ParsedCertificate* cert,
    const ParsedCertificate* issuer_certificate,
    const base::Time& verify_time,
    OCSPVerifyResult::ResponseStatus* response_details) {
  // The maximum age for an OCSP response, implemented as time since the
  // |this_update| field in OCSPSingleResponse. Responses older than |max_age|
  // will be considered invalid.
  base::TimeDelta max_age = base::TimeDelta::FromDays(7);
  OCSPRevocationStatus result = OCSPRevocationStatus::UNKNOWN;
  *response_details = OCSPVerifyResult::NO_MATCHING_RESPONSE;

  for (const auto& single_response_der : response_data.responses) {
    // In the common case, there should only be one SingleResponse in the
    // ResponseData (matching the certificate requested and used on this
    // connection). However, it is possible for the OCSP responder to provide
    // multiple responses for multiple certificates. Look through all the
    // provided SingleResponses, and check to see if any match the
    // certificate. A SingleResponse matches a certificate if it has the same
    // serial number, issuer name (hash), and issuer public key (hash).
    OCSPSingleResponse single_response;
    if (!ParseOCSPSingleResponse(single_response_der, &single_response))
      return OCSPRevocationStatus::UNKNOWN;
    OCSPCertID cert_id;
    if (!ParseOCSPCertID(single_response.cert_id_tlv, &cert_id))
      return OCSPRevocationStatus::UNKNOWN;
    if (!CheckCertIDMatchesCertificate(cert_id, cert, issuer_certificate))
      continue;

    // The SingleResponse matches the certificate, but may be out of date. Out
    // of date responses are noted seperate from responses with mismatched
    // serial numbers. If an OCSP responder provides both an up to date
    // response and an expired response, the up to date response takes
    // precedence (PROVIDED > INVALID_DATE).
    if (!CheckOCSPDateValid(single_response, verify_time, max_age)) {
      if (*response_details != OCSPVerifyResult::PROVIDED)
        *response_details = OCSPVerifyResult::INVALID_DATE;
      continue;
    }

    // In the case with multiple matching and up to date responses, keep only
    // the strictest status (REVOKED > UNKNOWN > GOOD).
    if (*response_details != OCSPVerifyResult::PROVIDED ||
        result == OCSPRevocationStatus::GOOD ||
        single_response.cert_status.status == OCSPRevocationStatus::REVOKED) {
      result = single_response.cert_status.status;
    }
    *response_details = OCSPVerifyResult::PROVIDED;
  }

  return result;
}

}  // namespace

OCSPRevocationStatus CheckOCSP(
    base::StringPiece raw_response,
    base::StringPiece certificate_der,
    base::StringPiece issuer_certificate_der,
    const base::Time& verify_time,
    OCSPVerifyResult::ResponseStatus* response_details) {
  *response_details = OCSPVerifyResult::NOT_CHECKED;

  if (raw_response.empty()) {
    *response_details = OCSPVerifyResult::MISSING;
    return OCSPRevocationStatus::UNKNOWN;
  }

  der::Input response_der(raw_response);
  OCSPResponse response;
  if (!ParseOCSPResponse(response_der, &response)) {
    *response_details = OCSPVerifyResult::PARSE_RESPONSE_ERROR;
    return OCSPRevocationStatus::UNKNOWN;
  }

  // RFC 6960 defines all responses |response_status| != SUCCESSFUL as error
  // responses. No revocation information is provided on error responses, and
  // the OCSPResponseData structure is not set.
  if (response.status != OCSPResponse::ResponseStatus::SUCCESSFUL) {
    *response_details = OCSPVerifyResult::ERROR_RESPONSE;
    return OCSPRevocationStatus::UNKNOWN;
  }

  // Actual revocation information is contained within the BasicOCSPResponse as
  // a ResponseData structure. The BasicOCSPResponse was parsed above, and
  // contains an unparsed ResponseData. From RFC 6960:
  //
  // BasicOCSPResponse       ::= SEQUENCE {
  //    tbsResponseData      ResponseData,
  //    signatureAlgorithm   AlgorithmIdentifier,
  //    signature            BIT STRING,
  //    certs            [0] EXPLICIT SEQUENCE OF Certificate OPTIONAL }
  //
  // ResponseData ::= SEQUENCE {
  //     version              [0] EXPLICIT Version DEFAULT v1,
  //     responderID              ResponderID,
  //     producedAt               GeneralizedTime,
  //     responses                SEQUENCE OF SingleResponse,
  //     responseExtensions   [1] EXPLICIT Extensions OPTIONAL }
  OCSPResponseData response_data;
  if (!ParseOCSPResponseData(response.data, &response_data)) {
    *response_details = OCSPVerifyResult::PARSE_RESPONSE_DATA_ERROR;
    return OCSPRevocationStatus::UNKNOWN;
  }

  scoped_refptr<ParsedCertificate> certificate =
      ParseCertificate(certificate_der);
  scoped_refptr<ParsedCertificate> issuer_certificate =
      ParseCertificate(issuer_certificate_der);

  if (!certificate || !issuer_certificate) {
    *response_details = OCSPVerifyResult::NOT_CHECKED;
    return OCSPRevocationStatus::UNKNOWN;
  }

  // If producedAt is outside of the certificate validity period, reject the
  // response.
  if (response_data.produced_at < certificate->tbs().validity_not_before ||
      response_data.produced_at > certificate->tbs().validity_not_after) {
    *response_details = OCSPVerifyResult::BAD_PRODUCED_AT;
    return OCSPRevocationStatus::UNKNOWN;
  }

  // Look through all of the OCSPSingleResponses for a match (based on CertID
  // and time).
  OCSPRevocationStatus status = GetRevocationStatusForCert(
      response_data, certificate.get(), issuer_certificate.get(), verify_time,
      response_details);

  // TODO(eroman): Process the OCSP extensions. In particular, must reject if
  // there are any critical extensions that are not understood.

  // Check that the OCSP response has a valid signature. It must either be
  // signed directly by the issuing certificate, or a valid authorized
  // responder.
  if (!VerifyOCSPResponseSignature(response, response_data,
                                   issuer_certificate.get())) {
    return OCSPRevocationStatus::UNKNOWN;
  }

  return status;
}

bool CheckOCSPDateValid(const OCSPSingleResponse& response,
                        const base::Time& verify_time,
                        const base::TimeDelta& max_age) {
  der::GeneralizedTime verify_time_der;
  if (!der::EncodeTimeAsGeneralizedTime(verify_time, &verify_time_der))
    return false;

  if (response.this_update > verify_time_der)
    return false;  // Response is not yet valid.

  if (response.has_next_update && (response.next_update <= verify_time_der))
    return false;  // Response is no longer valid.

  der::GeneralizedTime earliest_this_update;
  if (!der::EncodeTimeAsGeneralizedTime(verify_time - max_age,
                                        &earliest_this_update)) {
    return false;
  }
  if (response.this_update < earliest_this_update)
    return false;  // Response is too old.

  return true;
}

bool CreateOCSPRequest(const ParsedCertificate* cert,
                       const ParsedCertificate* issuer,
                       std::vector<uint8_t>* request_der) {
  request_der->clear();

  bssl::ScopedCBB cbb;

  // This initial buffer size is big enough for 20 octet long serial numbers
  // (upper bound from RFC 5280) and then a handful of extra bytes. This
  // number doesn't matter for correctness.
  const size_t kInitialBufferSize = 100;

  if (!CBB_init(cbb.get(), kInitialBufferSize))
    return false;

  //   OCSPRequest     ::=     SEQUENCE {
  //       tbsRequest                  TBSRequest,
  //       optionalSignature   [0]     EXPLICIT Signature OPTIONAL }
  //
  //   TBSRequest      ::=     SEQUENCE {
  //       version             [0]     EXPLICIT Version DEFAULT v1,
  //       requestorName       [1]     EXPLICIT GeneralName OPTIONAL,
  //       requestList                 SEQUENCE OF Request,
  //       requestExtensions   [2]     EXPLICIT Extensions OPTIONAL }
  CBB ocsp_request;
  if (!CBB_add_asn1(cbb.get(), &ocsp_request, CBS_ASN1_SEQUENCE))
    return false;

  CBB tbs_request;
  if (!CBB_add_asn1(&ocsp_request, &tbs_request, CBS_ASN1_SEQUENCE))
    return false;

  // "version", "requestorName", and "requestExtensions" are omitted.

  CBB request_list;
  if (!CBB_add_asn1(&tbs_request, &request_list, CBS_ASN1_SEQUENCE))
    return false;

  CBB request;
  if (!CBB_add_asn1(&request_list, &request, CBS_ASN1_SEQUENCE))
    return false;

  //   Request         ::=     SEQUENCE {
  //       reqCert                     CertID,
  //       singleRequestExtensions     [0] EXPLICIT Extensions OPTIONAL }
  CBB req_cert;
  if (!CBB_add_asn1(&request, &req_cert, CBS_ASN1_SEQUENCE))
    return false;

  //   CertID          ::=     SEQUENCE {
  //       hashAlgorithm       AlgorithmIdentifier,
  //       issuerNameHash      OCTET STRING, -- Hash of issuer's DN
  //       issuerKeyHash       OCTET STRING, -- Hash of issuer's public key
  //       serialNumber        CertificateSerialNumber }

  // TODO(eroman): Don't use SHA1.
  const EVP_MD* md = EVP_sha1();
  if (!EVP_marshal_digest_algorithm(&req_cert, md))
    return false;

  AppendHashAsOctetString(md, &req_cert, issuer->tbs().issuer_tlv);

  der::Input key_tlv;
  if (!GetSubjectPublicKeyBytes(issuer->tbs().spki_tlv, &key_tlv))
    return false;
  AppendHashAsOctetString(md, &req_cert, key_tlv);

  CBB serial_number;
  if (!CBB_add_asn1(&req_cert, &serial_number, CBS_ASN1_INTEGER))
    return false;
  if (!CBB_add_bytes(&serial_number, cert->tbs().serial_number.UnsafeData(),
                     cert->tbs().serial_number.Length())) {
    return false;
  }

  uint8_t* result_bytes;
  size_t result_bytes_length;
  if (!CBB_finish(cbb.get(), &result_bytes, &result_bytes_length))
    return false;
  bssl::UniquePtr<uint8_t> delete_tbs_cert_bytes(result_bytes);

  request_der->assign(result_bytes, result_bytes + result_bytes_length);
  return true;
}

// From RFC 2560 section A.1.1:
//
//    An OCSP request using the GET method is constructed as follows:
//
//    GET {url}/{url-encoding of base-64 encoding of the DER encoding of
//    the OCSPRequest}
GURL CreateOCSPGetURL(const ParsedCertificate* cert,
                      const ParsedCertificate* issuer,
                      const GURL& ocsp_responder_url) {
  std::vector<uint8_t> ocsp_request_der;
  if (!CreateOCSPRequest(cert, issuer, &ocsp_request_der)) {
    // Unexpected (means BoringSSL failed an operation).
    return GURL();
  }

  // Base64 encode the request data.
  std::string b64_encoded;
  base::Base64Encode(
      base::StringPiece(reinterpret_cast<const char*>(ocsp_request_der.data()),
                        ocsp_request_der.size()),
      &b64_encoded);

  // In theory +, /, and = are valid in paths and don't need to be escaped.
  // However from the example in RFC 5019 section 5 it is clear that the intent
  // is to escape non-alphanumeric characters (the example conclusively escapes
  // '/' and '=', but doesn't clarify '+').
  base::ReplaceSubstringsAfterOffset(&b64_encoded, 0, "+", "%2B");
  base::ReplaceSubstringsAfterOffset(&b64_encoded, 0, "/", "%2F");
  base::ReplaceSubstringsAfterOffset(&b64_encoded, 0, "=", "%3D");

  // RFC 2560 and RFC 5019 are vague on what is intended for URL concatenation.
  //
  //   * If the path doesn't end in a slash, is one implicitly added?
  //   * Is a straight up string concatenation expected, or only a concatenation
  //     to the path?
  //
  // This code contenates the data to the path portion of the URL, and leaves
  // the other URL components unmodified.
  //
  // TODO(eroman): Confirm whether OCSP responders use query parameters.
  std::string path = ocsp_responder_url.path();
  if (!base::StringPiece(path).ends_with("/"))
    path += "/";
  path += b64_encoded;

  GURL::Replacements replacements;
  replacements.SetPath(path.data(), url::Component(0, path.size()));
  return ocsp_responder_url.ReplaceComponents(replacements);
}

}  // namespace net

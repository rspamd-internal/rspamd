*** Settings ***
Suite Setup     Rspamd Setup
Suite Teardown  Rspamd Teardown
Library         ${RSPAMD_TESTDIR}/lib/rspamd.py
Resource        ${RSPAMD_TESTDIR}/lib/rspamd.robot
Variables       ${RSPAMD_TESTDIR}/lib/vars.py

*** Variables ***
${CONFIG}          ${RSPAMD_TESTDIR}/configs/dkim.conf
${RSPAMD_SCOPE}    Suite
${RSPAMD_URL_TLD}       ${RSPAMD_TESTDIR}/../lua/unit/test_tld.dat

*** Test Cases ***
DKIM PERMFAIL NXDOMAIN
  Scan File  ${RSPAMD_TESTDIR}/messages/dmarc/bad_dkim2.eml
  ...  IP=37.48.67.26
  Expect Symbol  R_DKIM_PERMFAIL

DKIM PERMFAIL BAD RECORD
  Scan File  ${RSPAMD_TESTDIR}/messages/dmarc/bad_dkim1.eml
  ...  IP=37.48.67.26
  Expect Symbol  R_DKIM_PERMFAIL

DKIM TEMPFAIL SERVFAIL UNALIGNED
  Scan File  ${RSPAMD_TESTDIR}/messages/dmarc/bad_dkim3.eml
  ...  IP=37.48.67.26
  Expect Symbol  R_DKIM_TEMPFAIL

DKIM NA NOSIG
  Scan File  ${RSPAMD_TESTDIR}/messages/utf.eml
  ...  IP=37.48.67.26
  Expect Symbol  R_DKIM_NA

DKIM Ignore Unknown Tags
  Scan File  ${RSPAMD_TESTDIR}/messages/dmarc/dkim_unknown_tags.eml
  Expect Symbol  R_DKIM_ALLOW

DKIM Sign
  Set Suite Variable  ${RAN_SIGNTEST}  0
  ${result} =  Scan Message With Rspamc  ${RSPAMD_TESTDIR}/messages/spam_message.eml  --mime  --header=dodkim=1
  Check Rspamc  ${result}  DKIM-Signature
  Set Suite Variable  ${SIGNED_MESSAGE}  ${RSPAMD_TMPDIR}/dkim_sign_test.eml
  Create File  ${SIGNED_MESSAGE}  ${result.stdout}
  Set Suite Variable  ${RAN_SIGNTEST}  1

DKIM Self Verify
  IF  ${RAN_SIGNTEST} == 0
    Fail  "Sign test was not run"
  END
  Scan File  ${SIGNED_MESSAGE}
  Expect Symbol  R_DKIM_ALLOW

DKIM Verify ED25519 PASS
  Scan File  ${RSPAMD_TESTDIR}/messages/ed25519.eml
  Expect Symbol  R_DKIM_ALLOW

DKIM Verify ED25519 REJECT
  Scan File  ${RSPAMD_TESTDIR}/messages/ed25519-broken.eml
  Expect Symbol  R_DKIM_REJECT

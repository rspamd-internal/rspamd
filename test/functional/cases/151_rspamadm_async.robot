*** Settings ***
Test Setup      Rspamadm test Setup
Test Teardown   Rspamadm test Teardown
Library         Process
Library         ${RSPAMD_TESTDIR}/lib/rspamd.py
Resource        ${RSPAMD_TESTDIR}/lib/rspamd.robot
Variables       ${RSPAMD_TESTDIR}/lib/vars.py

*** Variables ***
${CONFIG}          ${RSPAMD_TESTDIR}/configs/plugins.conf
${REDIS_SCOPE}     Test
# For dummy http
${RSPAMD_SCOPE}    Test
${RSPAMD_URL_TLD}  ${RSPAMD_TESTDIR}/../lua/unit/test_tld.dat

*** Test Cases ***
#Tcp client
#  ${result} =  Run Process  ${RSPAMADM}  lua  -b  ${RSPAMD_TESTDIR}/lua/rspamadm/test_tcp_client.lua
#  Should Match Regexp  ${result.stderr}  ^$
#  Should Be Equal As Integers  ${result.rc}  0
#  Should Be Equal  ${result.stdout}  hello post

Redis client
  ${result} =  Run Process  ${RSPAMADM}  lua  -b  ${RSPAMD_TESTDIR}/lua/rspamadm/test_redis_client.lua
  Should Match Regexp  ${result.stderr}  ^$
  Should Be Equal As Integers  ${result.rc}  0
  Should Be Equal  ${result.stdout}  true\thello from lua on redis

# Broken due to tmpdir override
#DNS client
#  ${result} =  Run Process  ${RSPAMADM}  --var\=CONFDIR\=${tmpdir}  lua  -b  ${RSPAMD_TESTDIR}/lua/rspamadm/test_dns_client.lua
#  Log  ${result.stdout}
#  Log  ${result.stderr}
#  Should Be Equal As Integers  ${result.rc}  0
#  Should Be Equal  ${result.stdout}  true\tk=ed25519; p=yi50DjK5O9pqbFpNHklsv9lqaS0ArSYu02qp1S0DW1Y=
#  Cleanup Temporary Directory  ${tmpdir}

*** Keywords ***

Rspamadm test Setup
  Run Dummy Http
  Run Redis

Rspamadm test Teardown
  Dummy Http Teardown
  Redis Teardown

Prepare temp directory
  [Arguments]  ${CONFIG}
  ${template} =  Get File  ${CONFIG}
  ${tmpdir} =  Make Temporary Directory
  ${config} =  Replace Variables  ${template}
  ${config} =  Replace Variables  ${config}
  Log  ${config}
  Create File  ${tmpdir}/rspamd.conf  ${config}
  [Return]  ${tmpdir}

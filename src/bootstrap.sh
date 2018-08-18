#!/bin/bash
#

set -x 

if [ -f @sysconfdir@/updown.conf ]; then
	exit
fi

# Bootstrap the database.
@PIPELINE_PREFIX@/libexec/pipeline/bootstrap

#
# Create default databse.
#
@PIPELINE_PREFIX@/libexec/pipeline/init.d start
@PIPELINE_PREFIX@/bin/createdb thurston start
@PIPELINE_PREFIX@/libexec/pipeline/init.d stop

#
#  Bootstrap services.
#
@BROKER_PREFIX@/libexec/broker/bootstrap
@NETP_PREFIX@/libexec/netp/bootstrap
@TLSPROXY_PREFIX@/libexec/tlsproxy/bootstrap
@FETCH_PREFIX@/libexec/fetch/bootstrap
@TRADER_PREFIX@/libexec/trader/bootstrap

#
# Fetch share data.
#

TMPDIR=$(mktemp -d /tmp/bootstrap.XXXXXX)
cd $TMPDIR
git clone $git/data.git $TMPDIR/data
mkdir @FETCH_PREFIX@/share/fetch/dialer
cp -a $TMPDIR/data/dialer/tessdata @FETCH_PREFIX@/share/fetch/dialer
rm -Rf $TMPDIR

mkdir @FETCH_PREFIX@/var/fetch/cvpp
sqlite3 @FETCH_PREFIX@/var/fetch/cvpp/cvpp.db < @FETCH_PREFIX@/share/fetch/cvpp.sql

#
# Allow services to talk to broker.
#
cat @NETP_PREFIX@/share/netp/cert.pem \
	@TLSPROXY_PREFIX@/share/tlsproxy/cert.pem \
	@FETCH_PREFIX@/share/fetch/cert.pem \
	@TRADER_PREFIX@/share/trader/cert.pem \
	>> @BROKER_PREFIX@/share/broker/verify.pem

cat @BROKER_PREFIX@/share/broker/cert.pem \
	>> @NETP_PREFIX@/share/netp/verify.pem

cat @BROKER_PREFIX@/share/broker/cert.pem \
	>> @TLSPROXY_PREFIX@/share/tlsproxy/verify.pem

cat @BROKER_PREFIX@/share/broker/cert.pem \
	>> @FETCH_PREFIX@/share/fetch/verify.pem

cat @BROKER_PREFIX@/share/broker/cert.pem \
	>> @TRADER_PREFIX@/share/trader/verify.pem

#
# Updown config file.
#
FORWARD=$(route | grep ^default | awk '{ print $8; }')

cat > @sysconfdir@/updown.conf <<-EOF
	OUTSIDE_FORWARD="$FORWARD"
	LIVE_PROTNET="prot"
	#LIVE_INTERFACES="em1"
	#LIVE_VPN=yes
EOF



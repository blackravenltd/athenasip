# AthenaSIP - Quick Start

1. Install AthenaSIP
2. Install [MySQL](https://www.mysql.com/) or [MariaDB](https://mariadb.org/)
3. Install [rtpproxy](https://www.rtpproxy.org/)
4. Install a SIP Client such as [LinPhone](https://www.linphone.org/) on two or more hosts.
5. Use your SIP clients to connect to the server using the accounts and passwords:

tom@sip.athenasip.org / testing123
jonny@sip.athenasip.org / testing123

6. Test calls between these accounts.

## TLS Snakeoil Certificates

To ease evaluation, a self-signed certificate/CA is included in `tls/`. To get common open source SIP clients to work with these certificates, follow these steps:

1. Modify your `/etc/hosts` file to point `sip.athenasip.org` to your local network address:

```
##
# Host Database
#
# localhost is used to configure the loopback interface
# when the system is booting.  Do not change this entry.
##
127.0.0.1	localhost
255.255.255.255	broadcasthost
::1             localhost

129.168.1.23    sip.athenasip.org
``` 

You should do this on each host where you will install a SIP client.

2. Modify the configuration of your SIP Phone to accept all server certificates. For LinPhone, change the following config key in `~/.linphonerc`:

```
[sip]

verify_server_certs=0
```

**WARNING: When you have finished evaluating AthenaSIP, you should restore this setting.**

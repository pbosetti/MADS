# Cryptographic communications

## Context

Since MADS v2.0.0, it is possible to setup the MADS network so that agents communicate over encripted channels.

We use the ZAP authentication protocolo and the CURVE encryption mechanism provided by [ZeroMQ](http://curvezmq.org). Elliptic-curve cryptography (ECC) is an approach to public-key cryptography based on the algebraic structure of elliptic curves over finite fields. ECC allows **smaller keys to provide equivalent security**, compared to cryptosystems based on modular exponentiation in finite fields, such as the RSA cryptosystem and ElGamal cryptosystem (see [here](http://www.nsa.gov/business/programs/elliptic_curve.shtml)).

Authentication and encryption work together and are **opional**. Effort has been taken to make them as easy as possible without compromising effectiveness.

Encritpion is provided by a pair of keys: 

* the public key is stored in a `.pub` file and can be safely moved around
* the private key is stored in a `.key` file and **shall never be copied or moved from the device where it has been generated**

## How it works

### Generating the keys

The `mads` command provides an option to generate a pair of keys:

```sh
mads --keypair=name
```

This generates a `name.key` (private) and a `name.pub` (public) pair of files. The public key can be safely copied and transmitted over the network. The private one must remain on the device where it has been generated.

### Launching the broker in crypto mode

The `mads-broker` must have its own private key and the publick keys of any client that wants to conect to the broker itself. The agents must have their own private/public pair, and the public key of the broker.

The broker and the agents search for keys in the same directory containing the `mads.ini` file, i.e. `$(mads -p)/etc`. The command line option `--keys_dir` (for both broker and agents) allows to specify a different directory to scan for keys.

The broker expects to find in said directory a pair of files named `broker.key` and `broker.pub`. The command line option `--crypto` takes an optional argument that is the name of the key files (if it is not `broker`). So, supposing that we generated the keys as `mads --keypair=mybroker` and then moved the keys to `/usr/local/keys`, by executing:

```sh
mads broker --crypto=mybroker --keys_dir=/usr/local/keys
```

we are using the key files `/usr/local/keys/mybroker.key` and `/usr/local/keys/mybroker.pub`, while with:

```sh
mads broker --crypto
```

we accepts all defaults.

### Launching the agents in crypto mode

For an agent to connect to a broker in crypto mode, you must:

* generate the keys for the agent with the name `client`
* put the private key in `$(mads -p)/etc` (typically)
* copy to the same directory the broker's public key
* copy the agent's public key to the broker machine/device and save it in the same `etc` directory
* **restart the broker**, so that it properly loads the public key

Then launch the agent with the `--crypto` argument (no options).

Note that: 

* the keys search directory can be overridden with the `--keys_dir` command line option
* the name of the broker key file can be overridden with the `--key_broker` option
* the name of the client key file can be overridden with the `--key_client` option

## Remote editing of the settings file

A broker can let editors read and replace its own settings file over the network. The service is off by default. Turn it on with `remote_edit = true` in the `[broker]` section and restart the broker. The other `[broker]` keys (`edit_address`, `editors_dir`, `edit_ipc`, `edit_backups`, `edit_password_file`) are described in **mads-broker**(1), section *REMOTE EDITING*. Editors use the *Open broker settings* panel of **mads-ini**(1), section *OPENING THE BROKER'S SETTINGS*.

### Access control

| | no password | password (`mads ini --set-password`) |
|---|---|---|
| no `--crypto` | no tcp listener, ipc only | plaintext tcp, password only; the file contents travel in clear |
| `--crypto` | CURVE, editor keys from `editors_dir` only; no editor key, no tcp listener | CURVE and password; editor keys, or bus keys when `editors_dir` has no valid key |

The ipc endpoint (on the broker's host, when the broker's `ipc` setting is on) is plaintext and never asks for a password. The owner-only runtime directory holding its socket is the access control: only processes of the broker's OS user can use it.

Editor keys are separate from the bus keys: an editor key gives no access to the bus, and an agent key in the keys directory gives no editing right (the one exception is the password case above).

### Setup

#### Broker host (ipc)

Nothing needs to be set up beyond:

```ini
[broker]
remote_edit = true
```

Restart the broker. On the same host, in **mads-ini**, use **Local broker** (the edit port, 9094 by default).

#### Development without keys (password over tcp)

For trusted networks only:

```sh
mads ini --set-password /path/to/mads.ini
```

The password is read twice from the terminal and is never stored in clear. Set `remote_edit = true` in the same file, restart the broker, then in **mads-ini** open `tcp://host:9094` (or just `host:9094`) with **Use CURVE keys** unchecked. The password is never sent and requests cannot be altered in transit, but the settings text, including any secrets in it, travels **in clear**. The broker prints a warning at startup.

#### Production with CURVE (editor keys)

On the editor's machine, generate an editor key pair:

```sh
mads --keypair NAME
```

This writes `NAME.key` and `NAME.pub` in the current directory. Copy `NAME.pub` to the broker's editors folder, `<keys_dir>/editors` by default (`$(mads -p)/etc/editors`), or to the `editors_dir` you set, then restart the broker. The broker does not create that folder: make it first if it does not exist. Without a valid key in it there is no tcp listener. Do not copy it to the keys directory itself, unless that editor should also join the bus.

The editor needs three files: `NAME.key`, `NAME.pub` and the broker's `broker.pub`. In **mads-ini**, enter the address, check **Use CURVE keys**, pick `NAME.key` with **Secret key...** and the broker's `.pub` with **Broker public key...**. Rooms that advertise CURVE use the same two files.

#### Production with CURVE and a password

This adds a second factor to the editor keys. Set up the editor keys as above, then:

```sh
mads ini --set-password /path/to/mads.ini
```

and restart the broker. The editor is asked for the password when it connects. If `editors_dir` holds no valid key, the broker accepts any bus key of the keys directory together with the password (its startup line then says "bus keys"): keep at least one editor key in `editors_dir` if bus participants should not be able to edit even when they know the password.

### Files and remembered passwords

* `<settings file>.edit_password` holds the password verifier: a salt, the Argon2id parameters and two derived SCRAM keys, never the password. It is created owner-only (mode 0600) and read by the broker at startup, so restart the broker after `mads ini --set-password` or `--remove-password`. The broker warns when it is readable by group or others, and treats a malformed file as if there were no password.
* Every save made through the service first copies the current file to `<settings file>.<YYYYmmdd-HHMMSS>.bak`, next to it. The newest `edit_backups` (10 by default) are kept.
* Every save and every refused or failed request is logged on the broker's console, with the editor's public key (or `ipc`).
* **Remember on this computer** (in the password prompt of **mads-ini**) stores a key derived from the password for that one broker endpoint, never the password. It goes in the editor's preferences file, owner-only. The derived key opens that broker only, and it stops working when the broker's password changes: the editor then drops it and asks again.

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
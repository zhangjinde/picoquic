/*
* Author: Christian Huitema
* Copyright (c) 2017, Private Octopus, Inc.
* All rights reserved.
*
* Permission to use, copy, modify, and distribute this software for any
* purpose with or without fee is hereby granted, provided that the above
* copyright notice and this permission notice appear in all copies.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
* ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL Private Octopus, Inc. BE LIABLE FOR ANY
* DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
* LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
* ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
* SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifdef _WINDOWS
#include "wincompat.h"
#endif
#include <string.h>
#include <stdio.h>
#include <openssl/pem.h>
#include "picotls.h"
#include "picotls/openssl.h"
#include "picoquic_internal.h"
#include "tls_api.h"

#define PICOQUIC_TRANSPORT_PARAMETERS_TLS_EXTENSION 26
#define PICOQUIC_TRANSPORT_PARAMETERS_MAX_SIZE 512

#define PICOQUIC_LABEL_0RTT "EXPORTER-QUIC 0-RTT Secret"
#define PICOQUIC_LABEL_1RTT_CLIENT "EXPORTER-QUIC client 1-RTT Secret"
#define PICOQUIC_LABEL_1RTT_SERVER "EXPORTER-QUIC server 1-RTT Secret"

typedef struct st_picoquic_tls_ctx_t {
	ptls_t *tls;
	picoquic_cnx_t * cnx;
	int client_mode;
	ptls_raw_extension_t ext[2];
	ptls_handshake_properties_t handshake_properties;
	ptls_iovec_t alpn_vec;
	uint8_t ext_data[128];
	uint8_t ext_received[128];
	size_t ext_received_length;
	int ext_received_return;
} picoquic_tls_ctx_t;

int picoquic_receive_transport_extensions(picoquic_cnx_t * cnx, int extension_mode,
	uint8_t * bytes, size_t bytes_max, size_t * consumed);

int picoquic_prepare_transport_extensions(picoquic_cnx_t * cnx, int extension_mode,
	uint8_t * bytes, size_t bytes_max, size_t * consumed);

static size_t picoquic_aead_decrypt_generic(uint8_t * output, uint8_t * input, size_t input_length,
    uint64_t seq_num, uint8_t * auth_data, size_t auth_data_length, void * aead_ctx);

int picoquic_server_setup_ticket_aead_contexts(picoquic_quic_t * quic,
    ptls_context_t *tls_ctx,
    const uint8_t * secret, size_t secret_length);

/*
 * Provide access to transport received transport extension for
 * logging purpose.
 */
void picoquic_provide_received_transport_extensions(picoquic_cnx_t * cnx,
	uint8_t ** ext_received,
	size_t * ext_received_length,
	int * ext_received_return,
	int * client_mode)
{
	picoquic_tls_ctx_t * ctx = cnx->tls_ctx;

	*ext_received = ctx->ext_received;
	*ext_received_length = ctx->ext_received_length;
	*ext_received_return = ctx->ext_received_return;
	*client_mode = ctx->client_mode;
}

/*
* Using the open ssl library to load the test certificate
*/

static X509* openPemFile(char const * filename)
{

    X509* cert = X509_new();
    BIO* bio_cert = BIO_new_file(filename, "rb");
    PEM_read_bio_X509(bio_cert, &cert, NULL, NULL);
    return cert;
}

static int get_certificates(char const * pem_fname, ptls_iovec_t ** list, size_t * nb_certs)
{
    int ret = 0;
    size_t count = 0;
    X509 *cert;
    ptls_iovec_t * certs;

    *nb_certs = 0;
    *list = NULL;

	certs = (ptls_iovec_t *)malloc(sizeof(ptls_iovec_t) * 16);

	if (certs == NULL)
	{
		ret = -1;
	}
	else
	{
		cert = openPemFile(pem_fname);
		memset(certs, 0, sizeof(ptls_iovec_t) * 16);

		if (cert == NULL)
		{
			ret = -1;
		}
		else
		{
			ptls_iovec_t *dst = &certs[count++];
			dst->len = i2d_X509(cert, &dst->base);
		}
	}
    *nb_certs = count;
    *list = certs;

    return ret;
}

static int SetSignCertificate(char const * keypem, ptls_context_t * ctx)
{
	int ret = 0;
    ptls_openssl_sign_certificate_t * signer;
	EVP_PKEY *pkey = EVP_PKEY_new();

	signer = (ptls_openssl_sign_certificate_t *)malloc(sizeof(ptls_openssl_sign_certificate_t));

	if (signer == NULL || pkey == NULL)
	{
		ret = -1;
	}
	else
	{
		BIO* bio_key = BIO_new_file(keypem, "rb");
		EVP_PKEY * ret_pkey = PEM_read_bio_PrivateKey(bio_key, &pkey, NULL, NULL);
		if (ret_pkey == NULL)
		{
			ret = -1;
		}
		else
		{
			ret = ptls_openssl_init_sign_certificate(signer, pkey);
		}
		ctx->sign_certificate = &signer->super;
	}

	if (pkey != NULL)
	{
		EVP_PKEY_free(pkey);
	}

	if (ret != 0 && signer != NULL)
	{
		free(signer);
	}

	return ret;
}

/* TODO: may want to provide a layer of isolation to not reveal
 * internal state of random number generator */
void picoquic_crypto_random(picoquic_quic_t * quic, void * buf, size_t len)
{
    ptls_context_t *ctx = (ptls_context_t *)quic->tls_master_ctx;

    ctx->random_bytes(buf, len);
}

uint64_t picoquic_crypto_uniform_random(picoquic_quic_t * quic, uint64_t rnd_max)
{
	uint64_t rnd;
	uint64_t rnd_min = ((uint64_t)((int64_t)-1)) % rnd_max;

	do {
		picoquic_crypto_random(quic, &rnd, sizeof(rnd));
	} while (rnd < rnd_min);

	return rnd%rnd_max;
}

/*
 * Non crypto public random generator. This is meant to provide good enough randomness
 * without disclosing the state of the crypto random number generator. This is
 * adequate for non critical random numbers, such as sequence numbers or padding.
 *
 * The following is an implementation of xorshift1024* suggested by Vigna.
 * The state must be seeded so that it is not everywhere zero. */

static uint64_t public_random_seed[16];
static int public_random_index = 0;

uint64_t picoquic_public_random_64(void) {
    const uint64_t s0 = public_random_seed[public_random_index++];
    uint64_t s1 = public_random_seed[public_random_index &= 15];
    s1 ^= s1 << 31; // a
    s1 ^= s1 >> 11; // b
    s1 ^= s0 ^ (s0 >> 30); // c
    public_random_seed[public_random_index] = s1;
    return s1 * (uint64_t)1181783497276652981;
}

void picoquic_public_random_seed(picoquic_quic_t * quic)
{
    picoquic_crypto_random(quic, public_random_seed, sizeof(public_random_seed));
    for (int i = 0; i < 16; i++)
    {
        (void)picoquic_public_random_64();
    }
}

void picoquic_public_random(void * buf, size_t len)
{
    uint8_t *x = buf;

    while (len > 0)
    {
        uint64_t y = picoquic_public_random_64();
        for (int i = 0; i < 8 && len > 0; i++)
        {
            *x++ = (uint8_t)( y & 255);
            y >>= 8;
            len--;
        }
    }
}

uint64_t picoquic_public_uniform_random(uint64_t rnd_max)
{
    uint64_t rnd;
    uint64_t rnd_min = ((uint64_t)((int64_t)-1)) % rnd_max;

    do {
        rnd = picoquic_public_random_64();
    } while (rnd < rnd_min);

    return rnd%rnd_max;
}

/*
 * The collect extensions call back is called by the picotls stack upon
 * reception of a handshake message containing extensions. It should return true (1)
 * if the stack can process the extension, false (0) otherwise.
 */

int picoquic_tls_collect_extensions_cb(ptls_t *tls, struct st_ptls_handshake_properties_t *properties, uint16_t type)
{
	return type == PICOQUIC_TRANSPORT_PARAMETERS_TLS_EXTENSION;
}


void picoquic_tls_set_extensions(picoquic_cnx_t * cnx, picoquic_tls_ctx_t *tls_ctx)
{
	size_t consumed;
	int ret = picoquic_prepare_transport_extensions(cnx, (tls_ctx->client_mode)?0:1,
		tls_ctx->ext_data, sizeof(tls_ctx->ext_data), &consumed);

	if (ret == 0)
	{
		tls_ctx->ext[0].type = PICOQUIC_TRANSPORT_PARAMETERS_TLS_EXTENSION;
		tls_ctx->ext[0].data.base = tls_ctx->ext_data;
		tls_ctx->ext[0].data.len = consumed;
		tls_ctx->ext[1].type = 0xFFFF;
		tls_ctx->ext[1].data.base = NULL;
		tls_ctx->ext[1].data.len = 0;
	}
	else
	{
		tls_ctx->ext[0].type = 0xFFFF;
		tls_ctx->ext[0].data.base = NULL;
		tls_ctx->ext[0].data.len = 0;
	}

	tls_ctx->handshake_properties.additional_extensions = tls_ctx->ext;
}

/*
 * The collected extensions call back is called by the stack upon
 * reception of a handshake message containing supported extensions.
 */

int picoquic_tls_collected_extensions_cb(ptls_t *tls, ptls_handshake_properties_t *properties,
	ptls_raw_extension_t *slots)
{
	int ret = 0;
	size_t consumed = 0;
	/* Find the context from the TLS context */
	picoquic_tls_ctx_t * ctx = (picoquic_tls_ctx_t *)
		((char *)properties - offsetof(struct st_picoquic_tls_ctx_t, handshake_properties));

	if (slots[0].type == PICOQUIC_TRANSPORT_PARAMETERS_TLS_EXTENSION && slots[1].type == 0xFFFF)
	{
		size_t copied_length = sizeof(ctx->ext_received);

		/* Retrieve the transport parameters */
		ret = picoquic_receive_transport_extensions(ctx->cnx, (ctx->client_mode)?1:0,
			slots[0].data.base, slots[0].data.len, &consumed);

		/* Copy the extensions in the local context for further debugging */
		ctx->ext_received_length = slots[0].data.len;
		if (copied_length > ctx->ext_received_length)
			copied_length = ctx->ext_received_length;
		memcpy(ctx->ext_received, slots[0].data.base, copied_length);
		ctx->ext_received_return = ret;
		/* For now, override the value in case of default */
		ret = 0;

		/* In server mode, only compose the extensions if properly received from client */
		if (ctx->client_mode == 0)
		{
			picoquic_tls_set_extensions(ctx->cnx, ctx);
		}
	}

	return ret;
}

/*
 * The Hello Call Back is called on the server side upon reception of the 
 * Client Hello. The picotls code will parse the client hello and retrieve
 * parameters such as SNI and proposed ALPN.
 * TODO: check the SNI in case several are supported.
 * TODO: check the ALPN in case several are supported.
 */

int picoquic_client_hello_call_back(ptls_on_client_hello_t * on_hello_cb_ctx,
	ptls_t *tls, ptls_iovec_t server_name, const ptls_iovec_t *negotiated_protocols,
	size_t num_negotiated_protocols, const uint16_t *signature_algorithms, size_t num_signature_algorithms)
{
	int alpn_found = 0;
	picoquic_quic_t ** ppquic = (picoquic_quic_t **)(
		((char*)on_hello_cb_ctx) + sizeof(ptls_on_client_hello_t));
	picoquic_quic_t * quic = *ppquic;

	/* Check if the client is proposing the expected ALPN */
	if (quic->default_alpn != NULL)
	{
		size_t len = strlen(quic->default_alpn);

		for (size_t i = 0; i < num_negotiated_protocols; i++)
		{
			if (negotiated_protocols[i].len == len &&
				memcmp(negotiated_protocols[i].base, quic->default_alpn, len) == 0)
			{
				alpn_found = 1;
				ptls_set_negotiated_protocol(tls, quic->default_alpn, len);
				break;
			}
		}
	}

	/* If no common ALPN found, pick the first choice of the client. 
	 * This could be problematic, but right now alpn use in quic is in flux.
	 */

	if (alpn_found == 0)
	{
		for (size_t i = 0; i < num_negotiated_protocols; i++)
		{
			if (negotiated_protocols[i].len > 0)
			{
				ptls_set_negotiated_protocol(tls,
					(char const *)negotiated_protocols[i].base, negotiated_protocols[i].len);
				break;
			}
		}
	}

	return 0;
}

/*
 * The server will generate session tickets if some parameters are set in the server
 * TLS context, including:
 *  - the session ticket encryption callback, defined per the "encrypt_ticket" member of the context.
 *  - the session ticket lifetime, defined per the "ticket_life_time" member of the context.
 * The encrypt call back is called on the server side when a session resume ticket is ready.
 * The call is:
 * cb(tls->ctx->encrypt_ticket, tls, 1, sendbuf,
 *    ptls_iovec_init(session_id.base, session_id.off))
 * The call to decrypt is:
 * tls->ctx->encrypt_ticket->cb(tls->ctx->encrypt_ticket, tls, 0, &decbuf, identity->identity)
 * Should return 0 if the ticket is good, etc.
 */

int picoquic_server_encrypt_ticket_call_back(ptls_encrypt_ticket_t * encrypt_ticket_ctx,
    ptls_t *tls, int is_encrypt, ptls_buffer_t *dst, ptls_iovec_t src)
{

    /* Assume that the keys are in the quic context 
     * The tickets are composed of a 64 bit "sequence number" 
     * followed by the result of the clear text encryption.
     */
    int ret = 0;
    picoquic_quic_t ** ppquic = (picoquic_quic_t **)(
        ((char*)encrypt_ticket_ctx) + sizeof(ptls_encrypt_ticket_t));
    picoquic_quic_t * quic = *ppquic;

    if (is_encrypt != 0)
    {
        ptls_aead_context_t * aead_enc = (ptls_aead_context_t *)quic->aead_encrypt_ticket_ctx;
        /* Encoding*/
        if (aead_enc == NULL)
        {
            ret = -1;
        }
        else if ((ret = ptls_buffer_reserve(dst, 8 + src.len + aead_enc->algo->tag_size)) == 0)
        {
            /* Create and store the ticket sequence number */
            uint64_t seq_num = picoquic_public_random_64();
            picoformat_64(dst->base + dst->off, seq_num);
            dst->off += 8;
            /* Run AEAD encryption */
            dst->off += ptls_aead_encrypt(aead_enc, dst->base + dst->off,
                src.base, src.len, seq_num, NULL, 0);
        }
    }
    else
    {
        ptls_aead_context_t * aead_dec = (ptls_aead_context_t *)quic->aead_decrypt_ticket_ctx;
        /* Encoding*/
        if (aead_dec == NULL)
        {
            ret = -1;
        }
        else if (src.len < 8 + aead_dec->algo->tag_size)
        {
            ret = -1;
        }
        else if ((ret = ptls_buffer_reserve(dst, src.len)) == 0)
        {
            /* Decode the ticket sequence number */
            uint64_t seq_num = PICOPARSE_64(src.base);
            /* Decrypt */
            size_t decrypted = ptls_aead_decrypt(aead_dec, dst->base + dst->off,
                src.base + 8, src.len - 8, seq_num, NULL, 0);

            if (decrypted > src.len - 8)
            {
                /* decryption error */
                ret = -1;
            }
            else
            {
                dst->off += decrypted;
            }
        }
    }

    return ret;
}

/*
 * The client signals its willingness to receive session resume tickets by providing
 * the "save ticket" callback in the client's quic context.
 */

int picoquic_client_save_ticket_call_back(ptls_save_ticket_t * save_ticket_ctx,
    ptls_t *tls, ptls_iovec_t input)
{
    int ret = 0;
    picoquic_quic_t * quic = *((picoquic_quic_t **)(
        ((char*)save_ticket_ctx) + sizeof(ptls_save_ticket_t)));
    const char * sni = ptls_get_server_name(tls);
    const char * alpn = ptls_get_negotiated_protocol(tls);

    if (sni != NULL && alpn != NULL)
    {
        ret = picoquic_store_ticket(&quic->p_first_ticket, 0, sni, (uint16_t)strlen(sni),
            alpn, (uint16_t)strlen(alpn), input.base, (uint16_t)input.len);
    }
    else
    {
        DBG_PRINTF("Received incorrect session resume ticket, sni = %s, alpn = %s, length = %d\n", 
            (sni == NULL)?"NULL":sni, (alpn == NULL)?"NULL":alpn, (int) input.len);
    }

    return ret;
}

/*
 * Time get callback
 */
uint64_t picoquic_get_simulated_time_cb(ptls_get_time_t *self)
{
    uint64_t ** pp_simulated_time = (uint64_t **)(((char *)self) + sizeof(ptls_get_time_t));
    return ((**pp_simulated_time)/1000);
}


/*
 * Setting the master TLS context.
 * On servers, this implies setting the "on hello" call back
 */

int picoquic_master_tlscontext(picoquic_quic_t * quic, 
    char const * cert_file_name, char const * key_file_name,
    const uint8_t * ticket_key, size_t ticket_key_length)
{
    /* Create a client context or a server context */
    int ret = 0;
    ptls_context_t *ctx;
    ptls_openssl_verify_certificate_t * verifier = NULL;
	ptls_on_client_hello_t * och = NULL;
    ptls_encrypt_ticket_t *encrypt_ticket = NULL;
    ptls_save_ticket_t *save_ticket = NULL;

    ctx = (ptls_context_t *)malloc(sizeof(ptls_context_t));

    if (ctx == NULL)
    {
        ret = -1;
    }
    else
    {
        memset(ctx, 0, sizeof(ptls_context_t));
        ctx->random_bytes = ptls_openssl_random_bytes;
        ctx->key_exchanges = ptls_openssl_key_exchanges;
        ctx->cipher_suites = ptls_openssl_cipher_suites;

        if (quic->p_simulated_time == NULL)
        {
            ctx->get_time = &ptls_get_time;
        }
        else
        {
            ptls_get_time_t * time_getter = (ptls_get_time_t *)malloc(sizeof(ptls_get_time_t) +
                sizeof(uint64_t *));
            if (time_getter == NULL)
            {
                ret = PICOQUIC_ERROR_MEMORY;
            }
            else
            {
                uint64_t ** pp_simulated_time = (uint64_t **)(((char *)time_getter) + sizeof(ptls_get_time_t));

                time_getter->cb = picoquic_get_simulated_time_cb;
                *pp_simulated_time = quic->p_simulated_time;
                ctx->get_time = time_getter;
            }
        }

        if (quic->flags&picoquic_context_server)
        {
            /* Read the certificate file */

            if (get_certificates(cert_file_name, &ctx->certificates.list, &ctx->certificates.count) != 0)
            {
                ret = -1;
            }
            else
            {
                ret = SetSignCertificate(key_file_name, ctx);
            }

            if (ret == 0)
            {
                och = (ptls_on_client_hello_t *)malloc(sizeof(ptls_on_client_hello_t) +
                    sizeof(picoquic_quic_t *));
                if (och != NULL)
                {
                    picoquic_quic_t ** ppquic = (picoquic_quic_t **)(
                        ((char*)och) + sizeof(ptls_on_client_hello_t));

                    och->cb = picoquic_client_hello_call_back;
                    ctx->on_client_hello = och;
                    *ppquic = quic;
                }
            }

            if (ret == 0)
            {
                ret = picoquic_server_setup_ticket_aead_contexts(quic, ctx, ticket_key, ticket_key_length);
            }
            
            if (ret == 0)
            {
                encrypt_ticket = (ptls_encrypt_ticket_t *)malloc(sizeof(ptls_encrypt_ticket_t) +
                    sizeof(picoquic_quic_t *));
                if (encrypt_ticket == NULL)
                {
                    ret = PICOQUIC_ERROR_MEMORY;
                }
                else
                {
                    picoquic_quic_t ** ppquic = (picoquic_quic_t **)(
                        ((char*)encrypt_ticket) + sizeof(ptls_encrypt_ticket_t));

                    encrypt_ticket->cb = picoquic_server_encrypt_ticket_call_back;
                    *ppquic = quic;

                    ctx->encrypt_ticket = encrypt_ticket;
                    ctx->ticket_lifetime = 100000; /* 100,000 seconds, a bit more than one day */
                    ctx->require_dhe_on_psk = 1;
                    ctx->max_early_data_size = 0xFFFFFFFF;
                }
            }
        }
        else
        {
            verifier = (ptls_openssl_verify_certificate_t *)malloc(sizeof(ptls_openssl_verify_certificate_t));
			if (verifier == NULL)
			{
				ctx->verify_certificate = NULL;
			}
			else
			{
				ptls_openssl_init_verify_certificate(verifier, NULL);
				ctx->verify_certificate = &verifier->super;
			}

            if (quic->ticket_file_name != NULL)
            {
                save_ticket = (ptls_save_ticket_t *)malloc(sizeof(ptls_save_ticket_t) +
                    sizeof(picoquic_quic_t *));
                if (save_ticket != NULL)
                {
                    picoquic_quic_t ** ppquic = (picoquic_quic_t **)(
                        ((char*)save_ticket) + sizeof(ptls_save_ticket_t));

                    save_ticket->cb = picoquic_client_save_ticket_call_back;
                    ctx->save_ticket = save_ticket;
                    *ppquic = quic;
                }
            }
        }

        if (ret == 0)
        {
            quic->tls_master_ctx = ctx;
            picoquic_public_random_seed(quic);
        }
        else
        {
            free(ctx);
        }
    }

    return ret;
}

void picoquic_master_tlscontext_free(picoquic_quic_t * quic)
{
	if (quic->tls_master_ctx != NULL)
	{
		ptls_context_t *ctx = (ptls_context_t *)quic->tls_master_ctx;

        if (quic->p_simulated_time != NULL && ctx->get_time != NULL)
        {
            free(ctx->get_time);
            ctx->get_time = NULL;
        }

		if (quic->flags&picoquic_context_server)
		{
			if (ctx->certificates.list != NULL)
			{
#if 0
				/* TODO: call proper openssl API to free the CERT */
				for (size_t i = 0; i < ctx->certificates.count; i++)
				{
					if (ctx->certificates.list[i].base != NULL)
					{
						free(ctx->certificates.list[i].base);
						ctx->certificates.list[i].base = NULL;
					}
					ctx->certificates.list[i].len = 0;
				}
#endif
				free(ctx->certificates.list);
			}
		}
		
		if (ctx->verify_certificate != NULL)
		{
			free(ctx->verify_certificate);
			ctx->verify_certificate = NULL;
		}

		if (ctx->on_client_hello != NULL)
		{
			free(ctx->on_client_hello);
		}

        if (ctx->encrypt_ticket != NULL)
        {
            free(ctx->encrypt_ticket);
        }
	}
}

/*
 * Creation of a TLS context.
 * This includes setting the handshake properties that will later be 
 * used during the TLS handshake.
 */
int picoquic_tlscontext_create(picoquic_quic_t * quic, picoquic_cnx_t * cnx, uint64_t current_time)
{
    int ret = 0;
	/* allocate a context structure */
	picoquic_tls_ctx_t * ctx = (picoquic_tls_ctx_t *)malloc(sizeof(picoquic_tls_ctx_t));

	/* Create the TLS context */
	if (ctx == NULL)
	{
		ret = -1;
	}
	else
	{
		memset(ctx, 0, sizeof(picoquic_tls_ctx_t));

		ctx->cnx = cnx;

		ctx->handshake_properties.collect_extension = picoquic_tls_collect_extensions_cb;
		ctx->handshake_properties.collected_extensions = picoquic_tls_collected_extensions_cb;
		ctx->client_mode = (quic->flags&picoquic_context_server) ? 0 : 1;

		ctx->tls = ptls_new((ptls_context_t *)quic->tls_master_ctx, 
			(ctx->client_mode)?0:1);

		if (ctx->tls == NULL)
		{
			free(ctx);
			ctx = NULL;
			ret = -1;
		}
		else if (ctx->client_mode)
		{
			if (cnx->sni != NULL)
			{
				ptls_set_server_name(ctx->tls, cnx->sni, strlen(cnx->sni));
			}

			if (cnx->alpn != NULL)
			{
				ctx->alpn_vec.base = (uint8_t *) cnx->alpn;
				ctx->alpn_vec.len = strlen(cnx->alpn);
				ctx->handshake_properties.client.negotiated_protocols.count = 1;
				ctx->handshake_properties.client.negotiated_protocols.list = &ctx->alpn_vec;
			}

			picoquic_tls_set_extensions(cnx, ctx);

            if (cnx->sni != NULL && cnx->alpn != NULL)
            {
                uint8_t * ticket = NULL;
                uint16_t ticket_length = 0;

                if (picoquic_get_ticket(cnx->quic->p_first_ticket, current_time,
                    cnx->sni, (uint16_t)strlen(cnx->sni), cnx->alpn, (uint16_t)strlen(cnx->alpn),
                    &ticket, &ticket_length) == 0)
                { 
                    ctx->handshake_properties.client.session_ticket.base = ticket;
                    ctx->handshake_properties.client.session_ticket.len = ticket_length;

                    ctx->handshake_properties.client.max_early_data_size = &cnx->max_early_data_size;

                    cnx->psk_cipher_suite_id = PICOPARSE_16(ticket + 8);
                }
            }
		}
        else
        {
            /* Enable server side HRR if cookie mode is required */
            if ((quic->flags&picoquic_context_check_cookie) != 0)
            {
                /* if the server should enforce the client to do a stateless retry */
                ctx->handshake_properties.server.cookie.enforce_use = 1;
                /* secret used for signing / verifying the cookie(internally uses HMAC) */
                ctx->handshake_properties.server.cookie.key = cnx->quic->retry_seed;
                /* additional data to be used for signing / verification */
                ctx->handshake_properties.server.cookie.additional_data.base
                    = (uint8_t *)&cnx->peer_addr;
                ctx->handshake_properties.server.cookie.additional_data.len
                    = cnx->peer_addr_len;
            }
        }
	}

	cnx->tls_ctx = (void *)ctx;

    return ret;
}

void picoquic_tlscontext_free(void * vctx)
{
	picoquic_tls_ctx_t * ctx = (picoquic_tls_ctx_t *)vctx;
	if (ctx->tls != NULL)
	{
		ptls_free((ptls_t *)ctx->tls);
		ctx->tls = NULL;
	}
	free(ctx);
}

char const * picoquic_tls_get_negotiated_alpn(picoquic_cnx_t * cnx)
{
	picoquic_tls_ctx_t * ctx = (picoquic_tls_ctx_t *)cnx->tls_ctx;

	return ptls_get_negotiated_protocol(ctx->tls);
}

char const * picoquic_tls_get_sni(picoquic_cnx_t * cnx)
{
	picoquic_tls_ctx_t * ctx = (picoquic_tls_ctx_t *)cnx->tls_ctx;

	return ptls_get_server_name(ctx->tls);
}

int picoquic_tls_is_psk_handshake(picoquic_cnx_t * cnx)
{
    /* int ret = cnx->is_psk_handshake; */
    int ret = ptls_is_psk_handshake(((picoquic_tls_ctx_t *)(cnx->tls_ctx))->tls);
    return ret;
}

/*
 * Arrival of a handshake item (frame 0) in a packet of type T.
 * This triggers an optional progress of the connection.
 * Different processing based on packet type:
 *
 * - Client side initialization. Include transport parameters.
 *   May provide 0-RTT initialisation.
 * - Client Initial Receive. Accept the connection. Include TP.
 *   May provide 0-RTT initialization.
 *   Provide 1-RTT init.
 * - Server Clear Text. Confirm the client side connection.
 *   May provide 1-RTT init
 */

int picoquic_tlsinput_segment(picoquic_cnx_t * cnx,
    uint8_t * bytes, size_t length, size_t * consumed, struct st_ptls_buffer_t * sendbuf)
{
	picoquic_tls_ctx_t * ctx = (picoquic_tls_ctx_t *)cnx->tls_ctx;
    size_t inlen = 0, roff = 0;
    int ret = 0;

    ptls_buffer_init(sendbuf, "", 0);

    /* Provide the data */
    while (roff < length && (ret == 0 || ret == PTLS_ERROR_IN_PROGRESS))
    {
        inlen = length - roff;
        if (ptls_handshake_is_complete(ctx->tls))
        {
            ret = ptls_receive(ctx->tls, sendbuf, bytes + roff, &inlen);
        }
        else
        {
            ret = ptls_handshake(ctx->tls, sendbuf, bytes + roff, &inlen, &ctx->handshake_properties);
        }
        roff += inlen;
    }

    *consumed = roff;

    return ret;
}

int picoquic_initialize_stream_zero(picoquic_cnx_t * cnx)
{
    int ret = 0;
    struct st_ptls_buffer_t sendbuf;
	picoquic_tls_ctx_t * ctx = (picoquic_tls_ctx_t *)cnx->tls_ctx;

    ptls_buffer_init(&sendbuf, "", 0);
    ret = ptls_handshake(ctx->tls, &sendbuf, NULL, NULL, &ctx->handshake_properties);

    if ((ret == 0 || ret == PTLS_ERROR_IN_PROGRESS))
    {
        if (sendbuf.off > 0)
        {
            ret = picoquic_add_to_stream(cnx, 0, sendbuf.base, sendbuf.off, 0);
        }
        ret = 0;

        picoquic_setup_0RTT_aead_contexts(cnx, 0);
    }
    else
    {
        ret = -1;
    }

    ptls_buffer_dispose(&sendbuf);

    return ret;
}

/*

Using function ptls_aead_new(cipher->aead, cipher->hash, is_enc, pp->secret);
is_enc == 0 => decryption key;
is_enc != 0 => encryption key;
returns * ptls_aead_context

To use:
size_t ptls_aead_encrypt(aead_context, void* output, void* input, size_t input_len,
64bit seq, auth_data, auth_data_length);

Similar for aead_decrypt
Decrypt returns size_t_max (-1) if decryption fails, number of bytes in output otherwise

*/

void picoquic_aead_free(void* aead_context)
{
    ptls_aead_free((ptls_aead_context_t *)aead_context);
}

size_t picoquic_aead_get_checksum_length(void* aead_context)
{
    return ((ptls_aead_context_t *)aead_context)->algo->tag_size;
}

/* Computation of the encryption and decryption methods for 0-RTT data */

int picoquic_setup_0RTT_aead_contexts(picoquic_cnx_t * cnx, int is_server)
{
    int ret = 0;
    uint8_t secret[256]; /* secret_max */
    picoquic_tls_ctx_t * ctx = (picoquic_tls_ctx_t *)cnx->tls_ctx;
    ptls_cipher_suite_t * cipher = ptls_get_cipher(ctx->tls);

    if (cipher == NULL)
    {
        ret = -1;
    }
    else if (cipher->hash->digest_size > sizeof(secret))
    {
        ret = PICOQUIC_ERROR_UNEXPECTED_ERROR;
    }
    else
    {
        /* Set up the encryption AEAD */
        ret = ptls_export_secret(ctx->tls, secret, cipher->hash->digest_size,
            PICOQUIC_LABEL_0RTT, ptls_iovec_init(NULL, 0), 1);

        if (ret == 0 && is_server == 0)
        {
            cnx->aead_0rtt_encrypt_ctx = (void *)
                ptls_aead_new(cipher->aead, cipher->hash, 1, secret);

            if (cnx->aead_0rtt_encrypt_ctx == NULL)
            {
                ret = PICOQUIC_ERROR_MEMORY;
            }
        }

        if (ret == 0)
        {
            cnx->aead_0rtt_decrypt_ctx = (void *)
                ptls_aead_new(cipher->aead, cipher->hash, 0, secret);
        }
    }

    return ret;
}


/* Computation of the encryption and decryption methods for 1-RTT data */
int picoquic_setup_1RTT_aead_contexts(picoquic_cnx_t * cnx, int is_server)
{
    int ret = 0;
    uint8_t secret[256]; /* secret_max */
    picoquic_tls_ctx_t * ctx = (picoquic_tls_ctx_t *)cnx->tls_ctx;
    ptls_cipher_suite_t * cipher = ptls_get_cipher(ctx->tls);

    if (cipher == NULL)
    {
        ret = -1;
    }
    else if (cipher->hash->digest_size > sizeof(secret))
    {
        ret = PICOQUIC_ERROR_UNEXPECTED_ERROR;
    }
    else
    {
        /* Set up the encryption AEAD */
        ret = ptls_export_secret(ctx->tls, secret, cipher->hash->digest_size,
            (is_server == 0) ? PICOQUIC_LABEL_1RTT_CLIENT : PICOQUIC_LABEL_1RTT_SERVER,
            ptls_iovec_init(NULL, 0), 0);

        if (ret == 0)
        {
            cnx->aead_encrypt_ctx = (void *)
                ptls_aead_new(cipher->aead, cipher->hash, 1, secret);

            if (cnx->aead_encrypt_ctx == NULL)
            {
                ret = PICOQUIC_ERROR_MEMORY;
            }

            cnx->aead_de_encrypt_ctx = (void *)
                ptls_aead_new(cipher->aead, cipher->hash, 0, secret);
        }

        /* Now set up the corresponding decryption */
        if (ret == 0)
        {
            ret = ptls_export_secret(ctx->tls, secret, cipher->hash->digest_size,
                (is_server != 0) ? PICOQUIC_LABEL_1RTT_CLIENT : PICOQUIC_LABEL_1RTT_SERVER,
                ptls_iovec_init(NULL, 0), 0);
        }

        if (ret == 0)
        {
            cnx->aead_decrypt_ctx = (void *)ptls_aead_new(cipher->aead, cipher->hash, 0, secret);

            if (cnx->aead_decrypt_ctx == NULL)
            {
                ret = -1;
            }
        }
    }

    return ret;
}

int picoquic_server_setup_ticket_aead_contexts(picoquic_quic_t * quic,
    ptls_context_t *tls_ctx,
    const uint8_t * secret, size_t secret_length)
{
    int ret = 0;
    uint8_t temp_secret[256]; /* secret_max */
    ptls_hash_algorithm_t * algo = &ptls_openssl_sha256;
    ptls_aead_algorithm_t * aead = &ptls_openssl_aes128gcm;

    if (algo->digest_size > sizeof(temp_secret))
    {
        ret = PICOQUIC_ERROR_UNEXPECTED_ERROR;
    }
    else
    {
        if (secret != NULL && secret_length > 0)
        {
            memset(temp_secret, 0, algo->digest_size);
            memcpy(temp_secret, secret, (secret_length > algo->digest_size) ? algo->digest_size : secret_length);
        }
        else
        {
            tls_ctx->random_bytes(temp_secret, algo->digest_size);
        }

        /* Create the AEAD contexts */
        quic->aead_encrypt_ticket_ctx = (void *)ptls_aead_new(aead, algo, 1, temp_secret);
        quic->aead_decrypt_ticket_ctx = (void *)ptls_aead_new(aead, algo, 0, temp_secret);

        if (quic->aead_encrypt_ticket_ctx == NULL || quic->aead_decrypt_ticket_ctx == NULL)
        {
            ret = PICOQUIC_ERROR_MEMORY;
        }

        /* erase the temporary secret */
        ptls_clear_memory(temp_secret, algo->digest_size);
    }

    return ret;
}

/* AEAD encrypt/decrypt routines */
static size_t picoquic_aead_decrypt_generic(uint8_t * output, uint8_t * input, size_t input_length,
    uint64_t seq_num, uint8_t * auth_data, size_t auth_data_length, void * aead_ctx)
{
    size_t decrypted = 0;

    if (aead_ctx == NULL)
    {
        decrypted = (uint64_t)(-1ll);
    }
    else
    {
        decrypted = ptls_aead_decrypt((ptls_aead_context_t *)aead_ctx,
            (void*)output, (const void *)input, input_length, seq_num,
            (void *)auth_data, auth_data_length);
    }

    return decrypted;
}

size_t picoquic_aead_decrypt(picoquic_cnx_t *cnx, uint8_t * output, uint8_t * input, size_t input_length,
    uint64_t seq_num, uint8_t * auth_data, size_t auth_data_length)
{
    return picoquic_aead_decrypt_generic(output, input, input_length, seq_num,
        auth_data, auth_data_length, cnx->aead_decrypt_ctx);
}

size_t picoquic_aead_de_encrypt(picoquic_cnx_t *cnx, uint8_t * output, uint8_t * input, size_t input_length,
    uint64_t seq_num, uint8_t * auth_data, size_t auth_data_length)
{
    return picoquic_aead_decrypt_generic(output, input, input_length, seq_num,
        auth_data, auth_data_length, cnx->aead_de_encrypt_ctx);
}

size_t picoquic_aead_0rtt_decrypt(picoquic_cnx_t *cnx, uint8_t * output, uint8_t * input, size_t input_length,
    uint64_t seq_num, uint8_t * auth_data, size_t auth_data_length)
{
    return picoquic_aead_decrypt_generic(output, input, input_length, seq_num,
        auth_data, auth_data_length, cnx->aead_0rtt_decrypt_ctx);
}

size_t picoquic_aead_cleartext_decrypt(picoquic_cnx_t *cnx, uint8_t * output, uint8_t * input, size_t input_length,
    uint64_t seq_num, uint8_t * auth_data, size_t auth_data_length)
{
    return picoquic_aead_decrypt_generic(output, input, input_length, seq_num,
        auth_data, auth_data_length, cnx->aead_decrypt_cleartext_ctx);
}

size_t picoquic_aead_cleartext_de_encrypt(picoquic_cnx_t *cnx, uint8_t * output, uint8_t * input, size_t input_length,
    uint64_t seq_num, uint8_t * auth_data, size_t auth_data_length)
{
    return picoquic_aead_decrypt_generic(output, input, input_length, seq_num,
        auth_data, auth_data_length, cnx->aead_de_encrypt_cleartext_ctx);
}

size_t picoquic_aead_encrypt_generic(uint8_t * output, uint8_t * input, size_t input_length,
    uint64_t seq_num, uint8_t * auth_data, size_t auth_data_length, void * aead_ctx)
{
    size_t encrypted = 0;

    encrypted = ptls_aead_encrypt((ptls_aead_context_t *)aead_ctx,
            (void*)output, (const void *)input, input_length, seq_num,
            (void *)auth_data, auth_data_length);

    return encrypted;
}

size_t picoquic_aead_encrypt(picoquic_cnx_t *cnx, uint8_t * output, uint8_t * input, size_t input_length,
    uint64_t seq_num, uint8_t * auth_data, size_t auth_data_length)
{
    return picoquic_aead_encrypt_generic(output, input, input_length, seq_num, 
        auth_data, auth_data_length, cnx->aead_encrypt_ctx);
}

size_t picoquic_aead_0rtt_encrypt(picoquic_cnx_t *cnx, uint8_t * output, uint8_t * input, size_t input_length,
    uint64_t seq_num, uint8_t * auth_data, size_t auth_data_length)
{
    return picoquic_aead_encrypt_generic(output, input, input_length, seq_num,
        auth_data, auth_data_length, cnx->aead_0rtt_encrypt_ctx);
}


size_t picoquic_aead_cleartext_encrypt(picoquic_cnx_t *cnx, uint8_t * output, uint8_t * input, size_t input_length,
    uint64_t seq_num, uint8_t * auth_data, size_t auth_data_length)
{
    return picoquic_aead_encrypt_generic(output, input, input_length, seq_num,
        auth_data, auth_data_length, cnx->aead_encrypt_cleartext_ctx);
}


/* Computation of the clear text AEAD encryption based on per version secret */
#define PICOQUIC_LABEL_HANDSHAKE_CLIENT "tls13 QUIC client handshake secret"
#define PICOQUIC_LABEL_HANDSHAKE_SERVER "tls13 QUIC server handshake secret"
#define PICOQUIC_LABEL_CLEAR_TEXT_KEY "tls13 key"
#define PICOQUIC_LABEL_CLEAR_TEXT_IV "tls13 iv"

uint8_t picoquic_cleartext_null_salt[] = {
    0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,
    0,0,0,0
};

/*
    cleartext_secret = HKDF-Extract(quic_version_1_salt,
    client_connection_id):
    Set hash algorithm to SHA-256, salt to the chosen value, ikm to the cnxid encoded on 8 bytes.
    int ptls_hkdf_extract(ptls_hash_algorithm_t *algo, void *output, ptls_iovec_t salt, ptls_iovec_t ikm)

    client_cleartext_secret =
    HKDF-Expand-Label(cleartext_secret,
        "QUIC client cleartext Secret",
        "", Hash.length)
    server_cleartext_secret =
    HKDF-Expand-Label(cleartext_secret,
        "QUIC server cleartext Secret",
        "", Hash.length)

        HKDF-Expand-Label uses HKDF-Expand [RFC5869] with a specially
        formatted info parameter, as shown:

        HKDF-Expand-Label(Secret, Label, HashValue, Length) =
        HKDF-Expand(Secret, HkdfLabel, Length)

        Where HkdfLabel is specified as:

        struct {
        uint16 length = Length;
        opaque label<10..255> = "tls13 " + Label;
        uint8 hashLength;     // Always 0
        } HkdfLabel;

*/

static size_t picoquic_setup_clear_text_aead_label(
    size_t out_len, uint8_t label[256], char const * label_text)
{
    size_t text_length = strlen(label_text);
    size_t byte_index = 0;

    if (text_length > 252)
    {
        /* There are practical limits to what we want to do */
        return 0;
    }
    picoformat_16(label, (uint16_t) out_len);
    byte_index += 2;
    label[byte_index++] = (uint8_t) text_length;
    memcpy(&label[byte_index], label_text, text_length);
    byte_index += text_length;
    label[byte_index++] = 0;

    return byte_index;
}

static void picoquic_setup_cleartext_aead_salt(size_t version_index, ptls_iovec_t * salt)
{ 
    if (picoquic_supported_versions[version_index].version_aead_key != NULL &&
        picoquic_supported_versions[version_index].version_aead_key_length > 0)
    {
        salt->base = picoquic_supported_versions[version_index].version_aead_key;
        salt->len = picoquic_supported_versions[version_index].version_aead_key_length;
    }
    else
    {
        salt->base = picoquic_cleartext_null_salt;
        salt->len = sizeof(picoquic_cleartext_null_salt);
    }
}

int picoquic_setup_cleartext_aead_contexts(picoquic_cnx_t * cnx, int is_server)
{
    int ret = 0;
    uint8_t master_secret[256]; /* secret_max */
    uint8_t cnx_id[8]; /* serialized cnx_id */
    ptls_hash_algorithm_t * algo = &ptls_openssl_sha256;
    ptls_aead_algorithm_t * aead = &ptls_openssl_aes128gcm;
    ptls_iovec_t salt;
    ptls_iovec_t ikm;
    uint8_t label[256];
    uint8_t client_secret[256];
    uint8_t server_secret[256];
    uint8_t * secret1, * secret2;
    ptls_iovec_t prk;
    ptls_iovec_t info;

    picoformat_64(cnx_id, cnx->initial_cnxid);
    picoquic_setup_cleartext_aead_salt(cnx->version_index, &salt);
    ikm.base = cnx_id;
    ikm.len = sizeof(cnx_id);

    /* Extract the master key -- key length will be 32 per SHA256 */
    ret = ptls_hkdf_extract(algo, master_secret, salt, ikm);
    if (ret == 0)
    {
        prk.base = master_secret;
        prk.len = algo->digest_size;
        /* Compose the client label */
        info.base = label;
        info.len = picoquic_setup_clear_text_aead_label(algo->digest_size,
            label, PICOQUIC_LABEL_HANDSHAKE_CLIENT);
        /* extract the client key */
        ret = ptls_hkdf_expand(algo, client_secret, algo->digest_size, prk, info);

        if (ret == 0)
        {
            /* Compose the server label */
            info.base = label;
            info.len = picoquic_setup_clear_text_aead_label(algo->digest_size, 
                label, PICOQUIC_LABEL_HANDSHAKE_SERVER);
            /* extract the server key */
            ret = ptls_hkdf_expand(algo, server_secret, algo->digest_size, prk, info);
        }
    }

    if (ret == 0)
    {
        if (is_server)
        {
            secret1 = server_secret;
            secret2 = client_secret;
        }
        else
        {
            secret1 = client_secret;
            secret2 = server_secret;
        }
        
        if (ret == 0)
        {
            /* Create the AEAD contexts */
            cnx->aead_encrypt_cleartext_ctx = (void *)
                ptls_aead_new(aead, algo, 1, secret1);
            cnx->aead_decrypt_cleartext_ctx = (void *)
                ptls_aead_new(aead, algo, 0, secret2);
            cnx->aead_de_encrypt_cleartext_ctx = (void *)
                ptls_aead_new(aead, algo, 0, secret1);
        }
    }

    return ret;
}


/* Input stream zero data to TLS context
 */

int picoquic_tlsinput_stream_zero(picoquic_cnx_t * cnx)
{
    int ret = 0;
    picoquic_stream_data * data = cnx->first_stream.stream_data;
    struct st_ptls_buffer_t sendbuf;

    if (data == NULL ||
        data->offset > cnx->first_stream.consumed_offset)
    {
        return 0;
    }

    ptls_buffer_init(&sendbuf, "", 0);

    while (
        (ret == 0 || ret == PTLS_ERROR_IN_PROGRESS) && 
        data != NULL && data->offset <= cnx->first_stream.consumed_offset)
    {
        size_t start = (size_t)(cnx->first_stream.consumed_offset - data->offset);
        size_t consumed = 0;

        ret = picoquic_tlsinput_segment(cnx, data->bytes + start,
            data->length - start, &consumed, &sendbuf);

        cnx->first_stream.consumed_offset += consumed;
        
        if (start + consumed >= data->length)
        {
            free(data->bytes);
            cnx->first_stream.stream_data = data->next_stream_data;
            free(data);
            data = cnx->first_stream.stream_data;
        }
    }

    if (ret == 0)
    {
        switch (cnx->cnx_state)
        {
		case picoquic_state_client_hrr_received:
			/* This is not supposed to happen -- HRR should generate "error in progress" */
			break;
        case picoquic_state_client_init:
		case picoquic_state_client_init_sent:
        case picoquic_state_client_renegotiate:
        case picoquic_state_client_init_resent:
        case picoquic_state_client_handshake_start:
        case picoquic_state_client_handshake_progress:
            /* Extract and install the client 1-RTT key */
            cnx->cnx_state = picoquic_state_client_almost_ready;
            ret = picoquic_setup_1RTT_aead_contexts(cnx, 0);
            break;
        case picoquic_state_server_init:
            /* Extract and install the server 0-RTT and 1-RTT key */
            picoquic_setup_0RTT_aead_contexts(cnx, 1);
            cnx->cnx_state = picoquic_state_server_almost_ready;
            ret = picoquic_setup_1RTT_aead_contexts(cnx, 1);
            break;
        case picoquic_state_client_almost_ready:
        case picoquic_state_handshake_failure:
        case picoquic_state_client_ready:
        case picoquic_state_server_almost_ready:
        case picoquic_state_server_ready:
        case picoquic_state_disconnecting:
        case picoquic_state_closing_received:
        case picoquic_state_closing:
        case picoquic_state_draining:
        case picoquic_state_disconnected:
            break;
        default:
            DBG_PRINTF("Unexpected connection state: %d\n", cnx->cnx_state);
            break;
        }
    }
    else if (ret == PTLS_ERROR_IN_PROGRESS && 
        (cnx->cnx_state == picoquic_state_client_init ||
            cnx->cnx_state == picoquic_state_client_init_sent ||
            cnx->cnx_state == picoquic_state_client_init_resent))
    {
        /* Extract and install the client 0-RTT key */
    }
	else if (ret == PTLS_ERROR_IN_PROGRESS &&
		(cnx->cnx_state == picoquic_state_client_hrr_received))
	{
		/* Need to reset the transport state of the connection */
		cnx->cnx_state = picoquic_state_client_init;
		/* Delete the packets queued for retransmission */
		while (cnx->retransmit_newest != NULL)
		{
			picoquic_dequeue_retransmit_packet(cnx, cnx->retransmit_newest, 1);
		}

		/* Reset the streams */
		picoquic_clear_stream(&cnx->first_stream);
		cnx->first_stream.consumed_offset = 0;
		cnx->first_stream.stream_flags = 0;
		cnx->first_stream.fin_offset = 0;
		cnx->first_stream.sent_offset = 0;
	}
    else if (ret == PTLS_ERROR_STATELESS_RETRY)
    {
        cnx->cnx_state = picoquic_state_server_send_hrr;
    }

    if ((ret == 0 || ret == PTLS_ERROR_IN_PROGRESS || ret == PTLS_ERROR_STATELESS_RETRY))
    {
        if (sendbuf.off > 0)
        {
            ret = picoquic_add_to_stream(cnx, 0, sendbuf.base, sendbuf.off, 0);
        }
        ret = 0;
    }
    else
    {
        ret = -1;
    }

    ptls_buffer_dispose(&sendbuf);

    return ret;
}

/*
 * Compute the 16 byte reset secret associated with a connection ID.
 * We implement it as the hash of a secret seed maintained per QUIC context
 * and the 8 bytes connection ID. 
 * This is written using PTLS portable hash API, initialized
 * for now with the OpenSSL implementation. Will have to adapt if we
 * decide to use the minicrypto API.
 */

int picoquic_create_cnxid_reset_secret(picoquic_quic_t * quic, uint64_t cnx_id,
	uint8_t reset_secret[PICOQUIC_RESET_SECRET_SIZE])
{
	/* Using OpenSSL for now: ptls_hash_algorithm_t ptls_openssl_sha256 */
	int ret = 0;
	ptls_hash_algorithm_t *algo = &ptls_openssl_sha256;
	ptls_hash_context_t *hash_ctx = algo->create();
	uint8_t final_hash[PTLS_MAX_DIGEST_SIZE];

	if (hash_ctx == NULL)
	{
		ret = -1;
		memset(reset_secret, 0, PICOQUIC_RESET_SECRET_SIZE);
	}
	else
	{
		hash_ctx->update(hash_ctx, quic->reset_seed, sizeof(quic->reset_seed));
		hash_ctx->update(hash_ctx, &cnx_id, sizeof(cnx_id));
		hash_ctx->final(hash_ctx, final_hash, PTLS_HASH_FINAL_MODE_FREE);
		memcpy(reset_secret, final_hash, PICOQUIC_RESET_SECRET_SIZE);
	}

	return(ret);
}

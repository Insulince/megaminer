package main

import (
	"bytes"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"net/http"
	"time"
)

// node is a minimal JSON-RPC client for a Bitcoin Knots node.
type node struct {
	url, user, pass string
	http            *http.Client
}

func newNode(url, user, pass string) *node {
	return &node{url: url, user: user, pass: pass, http: &http.Client{Timeout: 30 * time.Second}}
}

type rpcError struct {
	Code    int    `json:"code"`
	Message string `json:"message"`
}

// call invokes one RPC method and decodes its result into out (which may be
// nil to ignore the result).
func (n *node) call(out any, method string, params ...any) error {
	if params == nil {
		params = []any{}
	}
	body, err := json.Marshal(map[string]any{
		"jsonrpc": "1.0", "id": "bridge", "method": method, "params": params,
	})
	if err != nil {
		return err
	}
	req, err := http.NewRequest(http.MethodPost, n.url, bytes.NewReader(body))
	if err != nil {
		return err
	}
	req.SetBasicAuth(n.user, n.pass)
	req.Header.Set("Content-Type", "application/json")

	resp, err := n.http.Do(req)
	if err != nil {
		return fmt.Errorf("%s: %w", method, err)
	}
	defer resp.Body.Close()

	var reply struct {
		Result json.RawMessage `json:"result"`
		Error  *rpcError       `json:"error"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&reply); err != nil {
		return fmt.Errorf("%s: HTTP %s: %w", method, resp.Status, err)
	}
	if reply.Error != nil {
		return fmt.Errorf("%s: RPC error %d: %s", method, reply.Error.Code, reply.Error.Message)
	}
	if out == nil {
		return nil
	}
	return json.Unmarshal(reply.Result, out)
}

// blockTemplate is the part of getblocktemplate's result the bridge uses.
type blockTemplate struct {
	Version                  uint32            `json:"version"`
	PreviousBlockHash        string            `json:"previousblockhash"`
	Transactions             []json.RawMessage `json:"transactions"`
	CoinbaseValue            int64             `json:"coinbasevalue"`
	CurTime                  uint32            `json:"curtime"`
	Bits                     string            `json:"bits"`
	Height                   uint32            `json:"height"`
	DefaultWitnessCommitment string            `json:"default_witness_commitment"`
}

func (n *node) template() (*blockTemplate, error) {
	var t blockTemplate
	err := n.call(&t, "getblocktemplate", map[string]any{"rules": []string{"segwit", "blake2b"}})
	return &t, err
}

// submitBlock returns "" if the node accepted the block, or its reason for
// rejecting it.
func (n *node) submitBlock(block []byte) (reason string, err error) {
	var result *string
	if err := n.call(&result, "submitblock", hex.EncodeToString(block)); err != nil {
		return "", err
	}
	if result == nil {
		return "", nil
	}
	return *result, nil
}

func (n *node) bestBlockHash() (string, error) {
	var hash string
	err := n.call(&hash, "getbestblockhash")
	return hash, err
}

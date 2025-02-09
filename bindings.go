package reindexer

import (
	"context"
	"fmt"
	"reflect"
	"strconv"
	"time"

	"github.com/prometheus/client_golang/prometheus"
	otelattr "go.opentelemetry.io/otel/attribute"

	"github.com/restream/reindexer/v3/bindings"
	"github.com/restream/reindexer/v3/bindings/builtinserver/config"
	"github.com/restream/reindexer/v3/cjson"
)

const (
	modeInsert = bindings.ModeInsert
	modeUpdate = bindings.ModeUpdate
	modeUpsert = bindings.ModeUpsert
	modeDelete = bindings.ModeDelete
)

func (db *reindexerImpl) modifyItem(ctx context.Context, namespace string, ns *reindexerNamespace, item interface{}, json []byte, mode int, precepts ...string) (count int, err error) {

	if ns == nil {
		ns, err = db.getNS(namespace)
		if err != nil {
			return 0, err
		}
	}

	for tryCount := 0; tryCount < 2; tryCount++ {
		ser := cjson.NewPoolSerializer()
		defer ser.Close()

		format := 0
		stateToken := 0

		if format, stateToken, err = packItem(ns, item, json, ser); err != nil {
			return
		}

		out, err := db.binding.ModifyItem(ctx, ns.nsHash, ns.name, format, ser.Bytes(), mode, precepts, stateToken)

		if err != nil {
			rerr, ok := err.(bindings.Error)
			if ok && rerr.Code() == bindings.ErrStateInvalidated {
				db.query(ns.name).Limit(0).ExecCtx(ctx).Close()
				err = rerr
				continue
			}
			return 0, err
		}

		defer out.Free()

		rdSer := newSerializer(out.GetBuf())
		rawQueryParams := rdSer.readRawQueryParams(func(nsid int) {
			ns.cjsonState.ReadPayloadType(&rdSer.Serializer, db.binding, ns.name)
		})

		if rawQueryParams.count == 0 {
			return 0, err
		}

		resultp := rdSer.readRawtItemParams()

		ns.cacheItems.Remove(resultp.id)

		if len(precepts) > 0 && (resultp.cptr != 0 || resultp.data != nil) && reflect.TypeOf(item).Kind() == reflect.Ptr {
			nsArrEntry := nsArrayEntry{ns, ns.cjsonState.Copy()}
			if _, err := unpackItem(db.binding, &nsArrEntry, &resultp, false, true, item); err != nil {
				return 0, err
			}
		}

		return rawQueryParams.count, err
	}
	return 0, err
}

func packItem(ns *reindexerNamespace, item interface{}, json []byte, ser *cjson.Serializer) (format int, stateToken int, err error) {

	if item != nil {
		json, _ = item.([]byte)
	}

	if json == nil {
		t := reflect.TypeOf(item)
		if t.Kind() == reflect.Ptr {
			t = t.Elem()
		}
		if ns.rtype.Name() != t.Name() || ns.rtype.PkgPath() != t.PkgPath() {
			panic(ErrWrongType)
		}

		format = bindings.FormatCJson

		enc := ns.cjsonState.NewEncoder()
		if stateToken, err = enc.Encode(item, ser); err != nil {
			return
		}

	} else {
		format = bindings.FormatJson
		ser.Write(json)
	}

	return
}

func (db *reindexerImpl) getNS(namespace string) (*reindexerNamespace, error) {
	db.lock.RLock()
	defer db.lock.RUnlock()

	ns, ok := db.ns[namespace]
	if !ok {
		return nil, errNsNotFound
	}

	return ns, nil
}

func unpackItem(bin bindings.RawBinding, ns *nsArrayEntry, params *rawResultItemParams, allowUnsafe bool, nonCacheableData bool, item interface{}) (interface{}, error) {
	useCache := item == nil && (ns.deepCopyIface || allowUnsafe) && !nonCacheableData
	needCopy := ns.deepCopyIface && !allowUnsafe
	var err error

	if useCache && ns.cacheItems != nil {
		if citem, ok := ns.cacheItems.Get(params.id); ok && citem.version == params.version {
			item = citem.item
		} else {
			item = reflect.New(ns.rtype).Interface()
			dec := ns.localCjsonState.NewDecoder(item, bin)
			if params.cptr != 0 {
				err = dec.DecodeCPtr(params.cptr, item)
			} else if params.data != nil {
				err = dec.Decode(params.data, item)
			} else {
				panic(fmt.Errorf("Internal error while decoding item id %d from ns %s: cptr and data are both null", params.id, ns.name))
			}
			if err != nil {
				return item, err
			}

			if citem, ok := ns.cacheItems.Get(params.id); ok {
				if citem.version == params.version {
					item = citem.item
				} else if citem.version < params.version {
					ns.cacheItems.Add(params.id, &cacheItem{item: item, version: params.version})
				}
			} else {
				ns.cacheItems.Add(params.id, &cacheItem{item: item, version: params.version})
			}
		}
	} else {
		if item == nil {
			item = reflect.New(ns.rtype).Interface()
		}
		dec := ns.localCjsonState.NewDecoder(item, bin)
		if params.cptr != 0 {
			err = dec.DecodeCPtr(params.cptr, item)
		} else if params.data != nil {
			err = dec.Decode(params.data, item)
		} else {
			panic(fmt.Errorf("Internal error while decoding item id %d from ns %s: cptr and data are both null", params.id, ns.name))
		}
		if err != nil {
			return item, err
		}
		// Reset needCopy, because item already separate
		needCopy = false
	}

	if needCopy {
		if deepCopy, ok := item.(DeepCopy); ok {
			item = deepCopy.DeepCopy()
		} else {
			panic(fmt.Errorf("Internal error %s must implement DeepCopy interface", reflect.TypeOf(item).Name()))
		}
	}

	return item, err
}

func (db *reindexerImpl) rawResultToJson(rawResult []byte, jsonName string, totalName string, initJson []byte, initOffsets []int) (json []byte, offsets []int, explain []byte, err error) {

	ser := newSerializer(rawResult)
	rawQueryParams := ser.readRawQueryParams()
	explain = rawQueryParams.explainResults

	jsonReserveLen := len(rawResult) + len(totalName) + len(jsonName) + 20
	if cap(initJson) < jsonReserveLen {
		initJson = make([]byte, 0, jsonReserveLen)
	} else {
		initJson = initJson[:0]
	}
	jsonBuf := cjson.NewSerializer(initJson)

	if cap(initOffsets) < rawQueryParams.count {
		offsets = make([]int, 0, rawQueryParams.count)
	} else {
		offsets = initOffsets[:0]
	}

	jsonBuf.WriteString("{\"")

	if len(totalName) != 0 && rawQueryParams.totalcount != 0 {
		jsonBuf.WriteString(totalName)
		jsonBuf.WriteString("\":")
		jsonBuf.WriteString(strconv.Itoa(rawQueryParams.totalcount))
		jsonBuf.WriteString(",\"")
	}

	jsonBuf.WriteString(jsonName)
	jsonBuf.WriteString("\":[")

	for i := 0; i < rawQueryParams.count; i++ {
		item := ser.readRawtItemParams()
		if i != 0 {
			jsonBuf.WriteString(",")
		}
		offsets = append(offsets, len(jsonBuf.Bytes()))
		jsonBuf.Write(item.data)

		if (rawQueryParams.flags&bindings.ResultsWithJoined) != 0 && ser.GetVarUInt() != 0 {
			panic("Sorry, not implemented: Can't return join query results as json")
		}
	}
	jsonBuf.WriteString("]}")

	return jsonBuf.Bytes(), offsets, explain, nil
}

func (db *reindexerImpl) prepareQuery(ctx context.Context, q *Query, asJson bool) (result bindings.RawBuffer, err error) {

	if ns, err := db.getNS(q.Namespace); err == nil {
		q.nsArray = append(q.nsArray, nsArrayEntry{ns, ns.cjsonState.Copy()})
	} else {
		return nil, err
	}

	ser := q.ser
	for _, sq := range q.mergedQueries {
		if ns, err := db.getNS(sq.Namespace); err == nil {
			q.nsArray = append(q.nsArray, nsArrayEntry{ns, ns.cjsonState.Copy()})
		} else {
			return nil, err
		}
	}

	for _, sq := range q.joinQueries {
		if ns, err := db.getNS(sq.Namespace); err == nil {
			q.nsArray = append(q.nsArray, nsArrayEntry{ns, ns.cjsonState.Copy()})
		} else {
			return nil, err
		}
	}

	for _, mq := range q.mergedQueries {
		for _, sq := range mq.joinQueries {
			if ns, err := db.getNS(sq.Namespace); err == nil {
				q.nsArray = append(q.nsArray, nsArrayEntry{ns, ns.cjsonState.Copy()})
			} else {
				return nil, err
			}
		}
	}

	ser.PutVarCUInt(queryEnd)
	for _, sq := range q.joinQueries {
		ser.PutVarCUInt(sq.joinType)
		ser.Append(sq.ser)
		ser.PutVarCUInt(queryEnd)
	}

	for _, mq := range q.mergedQueries {
		ser.PutVarCUInt(merge)
		ser.Append(mq.ser)
		ser.PutVarCUInt(queryEnd)
		for _, sq := range mq.joinQueries {
			ser.PutVarCUInt(sq.joinType)
			ser.Append(sq.ser)
			ser.PutVarCUInt(queryEnd)
		}
	}

	for _, ns := range q.nsArray {
		q.ptVersions = append(q.ptVersions, ns.localCjsonState.Version^ns.localCjsonState.StateToken)
	}
	fetchCount := q.fetchCount
	if asJson {
		// json iterator not support fetch queries
		fetchCount = -1
	}
	result, err = db.binding.SelectQuery(ctx, ser.Bytes(), asJson, q.ptVersions, fetchCount)

	if err == nil && result.GetBuf() == nil {
		panic(fmt.Errorf("result.Buffer is nil"))
	}
	return
}

// Execute query
func (db *reindexerImpl) execQuery(ctx context.Context, q *Query) *Iterator {
	if db.otelTracer != nil {
		defer db.startTracingSpan(ctx, "Reindexer.Query.Exec", otelattr.String("rx.ns", q.Namespace)).End()
	}

	if db.promMetrics != nil {
		defer prometheus.NewTimer(db.promMetrics.clientCallsLatency.WithLabelValues("Query.Exec", q.Namespace)).ObserveDuration()
	}

	result, err := db.prepareQuery(ctx, q, false)
	if err != nil {
		return errIterator(err)
	}
	iter := newIterator(ctx, q.db, q.Namespace, q, result, q.nsArray, q.joinToFields, q.joinHandlers, q.context)
	return iter
}

func (db *reindexerImpl) execToJsonQuery(ctx context.Context, q *Query, jsonRoot string) *JSONIterator {
	if db.otelTracer != nil {
		defer db.startTracingSpan(ctx, "Reindexer.Query.ExecToJson", otelattr.String("rx.ns", q.Namespace)).End()
	}

	if q.db.promMetrics != nil {
		defer prometheus.NewTimer(q.db.promMetrics.clientCallsLatency.WithLabelValues("Query.ExecToJson", q.Namespace)).ObserveDuration()
	}

	result, err := db.prepareQuery(ctx, q, true)
	if err != nil {
		return errJSONIterator(err)
	}
	defer result.Free()
	var explain []byte
	q.json, q.jsonOffsets, explain, err = db.rawResultToJson(result.GetBuf(), jsonRoot, q.totalName, q.json, q.jsonOffsets)
	if err != nil {
		return errJSONIterator(err)
	}
	return newJSONIterator(ctx, q, q.json, q.jsonOffsets, explain)
}

func (db *reindexerImpl) prepareSQL(ctx context.Context, namespace, query string, asJson bool) (result bindings.RawBuffer, nsArray []nsArrayEntry, err error) {
	nsArray = make([]nsArrayEntry, 0, 3)
	var ns *reindexerNamespace

	if ns, err = db.getNS(namespace); err != nil {
		return
	}

	nsArray = append(nsArray, nsArrayEntry{ns, ns.cjsonState.Copy()})

	ptVersions := make([]int32, 0, 16)
	for _, ns := range nsArray {
		ptVersions = append(ptVersions, ns.localCjsonState.Version^ns.localCjsonState.StateToken)
	}

	result, err = db.binding.Select(ctx, query, asJson, ptVersions, defaultFetchCount)
	return
}

// Execute query
func (db *reindexerImpl) deleteQuery(ctx context.Context, q *Query) (int, error) {
	if db.otelTracer != nil {
		defer db.startTracingSpan(ctx, "Reindexer.Query.Delete", otelattr.String("rx.ns", q.Namespace)).End()
	}

	if db.promMetrics != nil {
		defer prometheus.NewTimer(db.promMetrics.clientCallsLatency.WithLabelValues("Query.Delete", q.Namespace)).ObserveDuration()
	}

	ns, err := db.getNS(q.Namespace)
	if err != nil {
		return 0, err
	}

	result, err := db.binding.DeleteQuery(ctx, ns.nsHash, q.ser.Bytes())
	if err != nil {
		return 0, err
	}
	defer result.Free()

	ser := newSerializer(result.GetBuf())
	// skip total count
	rawQueryParams := ser.readRawQueryParams()

	for i := 0; i < rawQueryParams.count; i++ {
		params := ser.readRawtItemParams()
		if (rawQueryParams.flags&bindings.ResultsWithJoined) != 0 && ser.GetVarUInt() != 0 {
			panic("Internal error: joined items in delete query result")
		}
		// Update cache
		ns.cacheItems.Remove(params.id)
	}
	if !ser.Eof() {
		panic("Internal error: data after end of delete query result")
	}

	return rawQueryParams.count, err
}

// Execute query
func (db *reindexerImpl) updateQuery(ctx context.Context, q *Query) *Iterator {
	if db.otelTracer != nil {
		defer db.startTracingSpan(ctx, "Reindexer.Query.Update", otelattr.String("rx.ns", q.Namespace)).End()
	}

	if db.promMetrics != nil {
		defer prometheus.NewTimer(db.promMetrics.clientCallsLatency.WithLabelValues("Query.Update", q.Namespace)).ObserveDuration()
	}

	ns, err := db.getNS(q.Namespace)
	if err != nil {
		return errIterator(err)
	}

	result, err := db.binding.UpdateQuery(ctx, ns.nsHash, q.ser.Bytes())
	if err != nil {
		return errIterator(err)
	}

	ser := newSerializer(result.GetBuf())
	// skip total count
	rawQueryParams := ser.readRawQueryParams(func(nsid int) {
		ns.cjsonState.ReadPayloadType(&ser.Serializer, db.binding, ns.name)
	})

	for i := 0; i < rawQueryParams.count; i++ {
		params := ser.readRawtItemParams()
		if (rawQueryParams.flags&bindings.ResultsWithJoined) != 0 && ser.GetVarUInt() != 0 {
			panic("Internal error: joined items in update query result")
		}
		// Update cache
		ns.cacheItems.Remove(params.id)
	}

	if !ser.Eof() {
		panic("Internal error: data after end of update query result")
	}

	q.nsArray = append(q.nsArray, nsArrayEntry{ns, ns.cjsonState.Copy()})
	return newIterator(ctx, q.db, q.Namespace, q, result, q.nsArray, nil, nil, nil)
}

// Execute query
func (db *reindexerImpl) updateQueryTx(ctx context.Context, q *Query, tx *Tx) *Iterator {
	if db.otelTracer != nil {
		defer db.startTracingSpan(ctx, "Reindexer.Tx.Query.Update", otelattr.String("rx.ns", q.Namespace)).End()
	}

	if db.promMetrics != nil {
		defer prometheus.NewTimer(db.promMetrics.clientCallsLatency.WithLabelValues("Tx.Query.Update", q.Namespace)).ObserveDuration()
	}

	err := db.binding.UpdateQueryTx(&tx.ctx, q.ser.Bytes())
	return errIterator(err)
}

// Execute query
func (db *reindexerImpl) deleteQueryTx(ctx context.Context, q *Query, tx *Tx) (int, error) {
	if db.otelTracer != nil {
		defer db.startTracingSpan(ctx, "Reindexer.Tx.Query.Delete", otelattr.String("rx.ns", q.Namespace)).End()
	}

	if db.promMetrics != nil {
		defer prometheus.NewTimer(db.promMetrics.clientCallsLatency.WithLabelValues("Tx.Query.Delete", q.Namespace)).ObserveDuration()
	}

	err := db.binding.DeleteQueryTx(&tx.ctx, q.ser.Bytes())
	return 0, err
}

func (db *Reindexer) ResetCaches() {
	db.impl.resetCachesCtx(db.ctx)
}

func (db *reindexerImpl) resetCachesCtx(ctx context.Context) {
	db.lock.RLock()
	nsArray := make([]*reindexerNamespace, 0, len(db.ns))
	for _, ns := range db.ns {
		nsArray = append(nsArray, ns)
	}
	db.lock.RUnlock()
	for _, ns := range nsArray {
		ns.cacheItems.Reset()
		ns.cjsonState.Reset()
		db.query(ns.name).Limit(0).ExecCtx(ctx).Close()
	}
}

func (db *reindexerImpl) resetCaches() {
	db.resetCachesCtx(context.Background())
}

func WithCgoLimit(cgoLimit int) interface{} {
	return bindings.OptionCgoLimit{CgoLimit: cgoLimit}
}

func WithConnPoolSize(connPoolSize int) interface{} {
	return bindings.OptionConnPoolSize{ConnPoolSize: connPoolSize}
}

func WithConnPoolLoadBalancing(algorithm bindings.LoadBalancingAlgorithm) interface{} {
	return bindings.OptionConnPoolLoadBalancing{Algorithm: algorithm}
}

func WithRetryAttempts(read int, write int) interface{} {
	return bindings.OptionRetryAttempts{Read: read, Write: write}
}

func WithServerConfig(startupTimeout time.Duration, serverConfig *config.ServerConfig) interface{} {
	return bindings.OptionBuiltinWithServer{ServerConfig: serverConfig, StartupTimeout: startupTimeout}
}

func WithTimeouts(loginTimeout time.Duration, requestTimeout time.Duration) interface{} {
	return bindings.OptionTimeouts{LoginTimeout: loginTimeout, RequestTimeout: requestTimeout}
}

func WithCreateDBIfMissing() interface{} {
	return bindings.OptionConnect{CreateDBIfMissing: true}
}

func WithNetCompression() interface{} {
	return bindings.OptionCompression{EnableCompression: true}
}

func WithDedicatedServerThreads() interface{} {
	return bindings.OptionDedicatedThreads{DedicatedThreads: true}
}

func WithAppName(appName string) interface{} {
	return bindings.OptionAppName{AppName: appName}
}

func WithPrometheusMetrics() interface{} {
	return bindings.OptionPrometheusMetrics{EnablePrometheusMetrics: true}
}

func WithOpenTelemetry() interface{} {
	return bindings.OptionOpenTelemetry{EnableTracing: true}
}

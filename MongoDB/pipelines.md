# MongoDB Pipelines

## Flatten Siemens Edge JSON data

```js
[
  /* Select a sub-type of messages */ 
  {
    $match: {
      "message.topic": {
        $regex: ".*",
        $options: "i"
      }
    }
  },
  /* Only take fields that matter */
  {
    $project: {
      timestamp: "$message.timestamp",
      msg_timecode: "$message.timecode",
      initial_timestamp: {
        timestamp: {
          $dateFromString: {
            dateString: "$message.payload.Header.Initial.Time"
          }
        },
        counter: "$message.payload.Header.Initial.HFProbeCounter"
      },
      timestep_ms: "$message.payload.Header.CycleTimeMs",
      type: {
        $first: "$message.payload.Header.Metadata.Value"
      },
      HFTimestamp: {
        time: {
          // $map applyes an operation to all elements of an array, returning 
          // a new array with the results
          $map: {
            input: "$message.payload.Payload.HFTimestamp.Time",
            as: "ts",
            in: {
              $dateFromString: {
                dateString: "$$ts"
              }
            }
          }
        },
        counter: "$message.payload.Payload.HFTimestamp.HFProbeCounter"
      },
      flat_data: {
        // $reduce applies an operation to all elements of an array, returning
        // a single value (accumulator)
        $reduce: {
          input: "$message.payload.Payload.HFData",
          initialValue: [],
          in: {
            $concatArrays: ["$$value", "$$this"]
          }
        }
      },
      data: "$message.payload.Payload.HFData"
    }
  },
  /* Unwind the data, i.e. extract ON document per each entry in the payload */
  {
    $unwind: {
      path: "$flat_data",
      includeArrayIndex: "id",
      preserveNullAndEmptyArrays: true
    }
  },
  /* Rearrange some fields. In particular, calculate the timestamp and 
   * timecode for each entry in the payload, based on the initial timestamp
   * and the timestep multiplied by the index of the entry in the payload
  */
  {
    $project: {
      timestamp: 1,
      msg_timecode: 1,
      initial_timestamp: 1,
      type: 1,
      time: {
        $dateAdd: {
          startDate: "$timestamp",
          unit: "millisecond",
          amount: {
            $function: {
              body: "function (dt, id) {return (dt * id);}",
              args: ["$timestep_ms", "$id"],
              lang: "js"
            }
          }
        }
      },
      /* This is tricky: js does not has a function for truncating at two 
       * digits, so we need to use toFixed, that returns a string, and convert 
       * back to a number.
       * NOTE: here we are using a tmedocde step of 40 ms, corresponding to 
       *       25 fps. This is a parameter that should be set according to the
       *       actual framerate of the acquisition system.
       */ 
      timecode: {
        $function: {
          body: "function(t0, dt, id) {return parseFloat((t0 + Math.round(dt * id / 40)*0.04).toFixed(2));}",
          args: [
            "$msg_timecode",
            "$timestep_ms",
            "$id"
          ],
          lang: "js"
        }
      },
      data: "$flat_data"
    }
  }
]
```
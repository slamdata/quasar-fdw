CREATE SERVER quasar FOREIGN DATA WRAPPER quasar_fdw
       OPTIONS (server 'http://localhost:8080'
               ,path '/local/quasar'
               ,timeout_ms '1001'
               ,use_remote_estimate 'false'
               ,fdw_startup_cost '101'
               ,fdw_tuple_cost '0.011');

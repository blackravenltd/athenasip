# AthenaSIP - Architecture

AthenaSIP is designed to hold only minimal internal state, so it can be clustered/load balanced efficiently in a 
cloud environment 

## Database

The database backend stores the AthenaSIP state, and requires an external database server. 

The following modules are currently available:

| Name         | Capabilities                    | Use Cases                             | Version | Status       |
|:-------------|:--------------------------------|:--------------------------------------|:--------|:-------------|
| db_mysql     | MySQL/MariaDB backend           | AthenaSIP Cluster                     | v0.0.1  | alpha        |           
| db_sqlite    | Internal SQLLite backend        | Single AthenaSIP instance, no cluster | -       | planned      |           
| db_postrgres | Postgres event backend          | AthenaSIP Cluster                     | -       | planned      |           
| db_redis     | Redis event backend             | AthenaSIP Cluster                     | -       | planned      |           
| db_dynamodb  | AWS DynamoDB event backend      | AthenaSIP Cluster                     | -       | planned      |           

## Eventing 

The event system is designed both to allow AthenaSIP to cluster efficiently and also handle SIP Presence 
([RFC3856](https://datatracker.ietf.org/doc/html/rfc3856) functionality), and requires either the default internal 
(in memory) driver or an external messaging server. Please note that AthenaSIP cannot cluster without using one of
the external eventing modules.

The following modules are currently available:

| Name         | Capabilities                    | Use Cases                             | Version | Status       |
|:-------------|:--------------------------------|:--------------------------------------|:--------|:-------------|
| evt_athena   | Internal (in memory) backend    | Single AthenaSIP instance, no cluster | v0.0.1  | alpha        |           
| evt_nats     | NATS event backend              | AthenaSIP Cluster                     | -       | planned      |           
| evt_rabbitmq | RabbitMQ event backend          | AthenaSIP Cluster                     | -       | planned      |           
| evt_nsq      | NSQ event backend               | AthenaSIP Cluster                     | -       | planned      |           
| evt_kafka    | Kafka event backend             | AthenaSIP Cluster                     | -       | planned      |           
| evt_redis    | Redis event backend             | AthenaSIP Cluster                     | -       | planned      |           
| evt_sqs      | AWS SQS event backend           | AthenaSIP Cluster                     | -       | planned      | 
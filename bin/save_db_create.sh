#!/bin/sh
mysqldump -u root --no-data --routines --triggers athenasip > sql/create.sql

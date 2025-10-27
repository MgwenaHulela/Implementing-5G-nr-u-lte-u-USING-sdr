import logging
import csv
import os
import random
import time
import pandas as pd
import simpy
import math

from dataclasses import dataclass, field
from typing import Dict, List

from .Times import *
from datetime import datetime

output_csv = "chap5/20_20/basic.csv"
file_log_name = f"{datetime.today().strftime('%Y-%m-%d-%H-%M-%S')}.log"

typ_filename = "RS_coex_1sta_1wifi2.log"

logging.basicConfig(filename="",
                    format='%(asctime)s %(message)s',
                    filemode='w ')
logger = logging.getLogger()
logger.setLevel(logging.CRITICAL)  # chose DEBUG to display stats in debug mode :)

colors = [
    "\033[30m",
    "\033[32m",
    "\033[31m",
    "\033[33m",
    "\033[34m",
    "\033[35m",
    "\033[36m",
    "\033[37m",
]  # colors to distinguish stations in output

big_num = 100000  # some big number for quesing in peeemtive resources - big starting point

gap = True


class Channel_occupied(Exception):
    pass


@dataclass()
class Config:
    data_size: int = 1472  # size od payload in b
    cw_min: int = 15  # min cw window size
    cw_max: int = 63  # max cw window size 1023 def
    r_limit: int = 7
    mcs: int = 7

@dataclass
class DynamicCWController:
    """Controls dynamic CW adjustment for fairness"""
    env: simpy.Environment
    channel: 'Channel'
    wifi_config: Config
    nru_config: Config
    
    # Controller parameters
    measurement_interval: int = 1000000  # 1 second in microseconds
    adjustment_step: int = 5  # How much to change CW
    target_fairness: float = 0.95  # Target Jain's fairness index
    min_cw: int = 7
    max_cw: int = 511
    
    # State tracking
    last_wifi_airtime: float = 0
    last_nru_airtime: float = 0
    last_measurement_time: int = 0
    adjustment_history: List = field(default_factory=list)
    
    def start_monitoring(self):
        """Start the monitoring process"""
        self.env.process(self._monitor_and_adjust())
    
    def _monitor_and_adjust(self):
        """Periodically measure and adjust CW values"""
        while True:
            yield self.env.timeout(self.measurement_interval)
            
            # Calculate current airtime for each technology
            current_wifi_airtime = sum(self.channel.airtime_data.values())
            current_nru_airtime = sum(self.channel.airtime_data_NR.values())
            
            # Calculate airtime in this period
            wifi_delta = current_wifi_airtime - self.last_wifi_airtime
            nru_delta = current_nru_airtime - self.last_nru_airtime
            
            # Calculate Jain's fairness index
            if wifi_delta > 0 or nru_delta > 0:
                fairness = self._calculate_jains_fairness(wifi_delta, nru_delta)
                
                # Adjust CW if fairness is below target
                if fairness < self.target_fairness:
                    self._adjust_contention_windows(wifi_delta, nru_delta, fairness)
            
            # Update state
            self.last_wifi_airtime = current_wifi_airtime
            self.last_nru_airtime = current_nru_airtime
            self.last_measurement_time = self.env.now
    
    def _calculate_jains_fairness(self, wifi_airtime: float, nru_airtime: float) -> float:
        """Calculate Jain's fairness index for two technologies"""
        if wifi_airtime == 0 and nru_airtime == 0:
            return 1.0
        
        sum_airtime = wifi_airtime + nru_airtime
        sum_squared = wifi_airtime**2 + nru_airtime**2
        
        if sum_squared == 0:
            return 1.0
            
        return (sum_airtime ** 2) / (2 * sum_squared)
    
    def _adjust_contention_windows(self, wifi_airtime: float, nru_airtime: float, 
                                   current_fairness: float):
        """Adjust CW values to improve fairness"""
        # Determine which technology is getting more airtime
        if wifi_airtime > nru_airtime * 1.1:  # WiFi getting >10% more
            # Increase WiFi CW to slow it down
            self._increase_wifi_cw()
            # Optionally decrease NRU CW
            if self.nru_config.cw_min > self.min_cw:
                self._decrease_nru_cw()
                
        elif nru_airtime > wifi_airtime * 1.1:  # NRU getting >10% more
            # Decrease WiFi CW to speed it up
            if self.wifi_config.cw_min > self.min_cw:
                self._decrease_wifi_cw()
            # Increase NRU CW to slow it down
            self._increase_nru_cw()
        
        # Log adjustment
        self.adjustment_history.append({
            'time': self.env.now,
            'wifi_cw_min': self.wifi_config.cw_min,
            'wifi_cw_max': self.wifi_config.cw_max,
            'nru_cw_min': self.nru_config.cw_min,
            'nru_cw_max': self.nru_config.cw_max,
            'fairness': current_fairness,
            'wifi_airtime': wifi_airtime,
            'nru_airtime': nru_airtime
        })
    
    def _increase_wifi_cw(self):
        """Increase WiFi contention window"""
        new_cw = min(self.wifi_config.cw_min + self.adjustment_step, self.max_cw)
        self.wifi_config.cw_min = new_cw
        self.wifi_config.cw_max = min(new_cw * 4, self.max_cw)
        
        # Update all active WiFi stations
        for station in self.channel.stations.values():
            station.cw_min = self.wifi_config.cw_min
            station.cw_max = self.wifi_config.cw_max
    
    def _decrease_wifi_cw(self):
        """Decrease WiFi contention window"""
        new_cw = max(self.wifi_config.cw_min - self.adjustment_step, self.min_cw)
        self.wifi_config.cw_min = new_cw
        self.wifi_config.cw_max = min(new_cw * 4, self.max_cw)
        
        # Update all active WiFi stations
        for station in self.channel.stations.values():
            station.cw_min = self.wifi_config.cw_min
            station.cw_max = self.wifi_config.cw_max
    
    def _increase_nru_cw(self):
        """Increase NRU contention window"""
        new_cw = min(self.nru_config.cw_min + self.adjustment_step, self.max_cw)
        self.nru_config.cw_min = new_cw
        self.nru_config.cw_max = min(new_cw * 4, self.max_cw)
        
        # Update all active NRU gNBs
        for gnb in self.channel.gnbs.values():
            gnb.cw_min = self.nru_config.cw_min
            gnb.cw_max = self.nru_config.cw_max
    
    def _decrease_nru_cw(self):
        """Decrease NRU contention window"""
        new_cw = max(self.nru_config.cw_min - self.adjustment_step, self.min_cw)
        self.nru_config.cw_min = new_cw
        self.nru_config.cw_max = min(new_cw * 4, self.max_cw)
        
        # Update all active NRU gNBs
        for gnb in self.channel.gnbs.values():
            gnb.cw_min = self.nru_config.cw_min
            gnb.cw_max = self.nru_config.cw_max

@dataclass()
class Config_NR:
    deter_period: int = 16  # time used for waiting in prioritization period, microsec
    observation_slot_duration: int = 9  # observation slot in mikros
    synchronization_slot_duration: int = 1000  # synchronization slot lenght in mikros
    max_sync_slot_desync: int = 1000
    min_sync_slot_desync: int = 0
    # channel access class related:
    M: int = 3  # amount of observation slots to wait after deter perion in prioritization period
    cw_min: int = 15
    cw_max: int = 63
    mcot: int = 6  # max ocupancy time


def random_sample(max, number, min_distance=0):  # func used to desync gNBs
    # returns number * elements <0, max>
    samples = random.sample(range(max - (number - 1) * (min_distance - 1)), number)
    indices = sorted(range(len(samples)), key=lambda i: samples[i])
    ranks = sorted(indices, key=lambda i: indices[i])
    return [sample + (min_distance - 1) * rank for sample, rank in zip(samples, ranks)]


def log(gnb, mes: str) -> None:
    logger.info(
        f"{gnb.col}Time: {gnb.env.now} Station: {gnb.name} Message: {mes}"
    )


class Station:
    def __init__(
            self,
            env: simpy.Environment,
            name: str,
            channel: dataclass,
            config: Config = Config(),
    ):
        self.config = config
        self.times = Times(config.data_size, config.mcs)  # using Times script to get time calculations
        self.name = name  # name of the station
        self.env = env  # simpy environment
        self.col = random.choice(colors)  # color of output -- for future station distinction
        self.frame_to_send = None  # the frame object which is next to send
        self.succeeded_transmissions = 0  # all succeeded transmissions for station
        self.failed_transmissions = 0  # all failed transmissions for station
        self.failed_transmissions_in_row = 0  # all failed transmissions for station in a row
        self.cw_min = config.cw_min  # cw min parameter value
        self.cw_max = config.cw_max  # cw max parameter value
        self.channel = channel  # channel obj
        env.process(self.start())  # starting simulation process
        self.process = None  # waiting back off process
        self.channel.airtime_data.update({name: 0})
        self.channel.airtime_control.update({name: 0})
        self.first_interrupt = False
        self.back_off_time = 0
        self.start = 0
        self.total_latency = 0  # Sum of all packet latencies
        self.packet_count = 0   # Number of packets for latency calculation
        self.transmit_power_dbm = 23  # Transmit power in dBm
        self.next_packet_id = 0  # For tracking packets
        self.packet_queue = []  # Queue of packets waiting to be sent
        self.is_transmitting = False
    def create_and_queue_packet(self):
        new_packet = Packet(id=self.next_packet_id, gen_time=self.env.now, payload_size=self.config.data_size)
        self.packet_queue.append(new_packet)
        self.next_packet_id += 1
        
   
    
    def start(self):
        while True:
            self.frame_to_send = self.generate_new_frame()
            was_sent = False
            while not was_sent:
                self.process = self.env.process(self.wait_back_off())
                yield self.process
                # self.process = None
                was_sent = yield self.env.process(self.send_frame())
                # self.process = None

    def wait_back_off(self):
        #global start
        self.back_off_time = self.generate_new_back_off_time(
            self.failed_transmissions_in_row)  # generating the new Back Off time

        while self.back_off_time > -1:
            try:
                with self.channel.tx_lock.request() as req:  # waiting  for idle channel -- empty channel
                    yield req
                self.back_off_time += Times.t_difs  # add DIFS time
                log(self, f"Starting to wait backoff (with DIFS): ({self.back_off_time})u...")
                self.first_interrupt = True
                self.start = self.env.now  # store the current simulation time
                self.channel.back_off_list.append(self)  # join the list off stations which are waiting Back Offs

                yield self.env.timeout(self.back_off_time)  # join the environment action queue

                log(self, f"Backoff waited, sending frame...")
                self.back_off_time = -1  # leave the loop

                self.channel.back_off_list.remove(self)  # leave the waiting list as Backoff was waited successfully

            except simpy.Interrupt:  # handle the interruptions from transmitting stations
                if self.first_interrupt and self.start is not None:
                    #tak jest po mojemu:
                    log(self, "Waiting was interrupted, waiting to resume backoff...")
                    all_waited = self.env.now - self.start
                    if all_waited <= Times.t_difs:
                        self.back_off_time -= Times.t_difs
                        log(self, f"Interupted in DIFS ({Times.t_difs}), backoff {self.back_off_time}, already waited: {all_waited}")
                    else:
                        back_waited = all_waited - Times.t_difs
                        slot_waited = int(back_waited / Times.t_slot)
                        self.back_off_time -= ((slot_waited * Times.t_slot) + Times.t_difs)
                        log(self,
                            f"Completed slots(9us) {slot_waited} = {(slot_waited * Times.t_slot)}  plus DIFS time {Times.t_difs}")
                        log(self,
                            f"Backoff decresed by {((slot_waited * Times.t_slot) + Times.t_difs)} new Backoff {self.back_off_time}")
                    self.first_interrupt = False

    def send_frame(self):
        self.channel.tx_list.append(self) 
        self.is_transmitting = True # add station to currently transmitting list
        res = self.channel.tx_queue.request(
            priority=(big_num - self.frame_to_send.frame_time))  # create request basing on this station frame length

        try:
            result = yield res | self.env.timeout(
                0)  # try to hold transmitting lock(station with the longest frame will get this)
            if res not in result:  # check if this station got lock, if not just wait you frame time
                raise simpy.Interrupt("There is a longer frame...")


            with self.channel.tx_lock.request() as lock:  # this station has the longest frame so hold the lock
                yield lock


                for station in self.channel.back_off_list:  # stop all station which are waiting backoff as channel is not idle
                    if station.process.is_alive:
                        station.process.interrupt()
                for gnb in self.channel.back_off_list_NR:  # stop all station which are waiting backoff as channel is not idle
                    if gnb.process.is_alive:
                        gnb.process.interrupt()

                log(self, f'Starting sending frame: {self.frame_to_send.frame_time}')

                yield self.env.timeout(self.frame_to_send.frame_time)  # wait this station frame time
                self.channel.back_off_list.clear()  # channel idle, clear backoff waiting list
                was_sent = self.check_collision()  # check if collision occurred

                if was_sent:  # transmission successful
                    self.channel.airtime_control[self.name] += self.times.get_ack_frame_time()
                    yield self.env.timeout(self.times.get_ack_frame_time())  # wait ack
                    self.channel.tx_list.clear()  # clear transmitting list
                    self.channel.tx_list_NR.clear()
                    self.channel.tx_queue.release(res)  # leave the transmitting queue
                    return True

                # there was collision
                self.channel.tx_list.clear()  # clear transmitting list
                self.channel.tx_list_NR.clear()
                self.channel.tx_queue.release(res)  # leave the transmitting queue
                self.channel.tx_queue = simpy.PreemptiveResource(self.env,
                                                                 capacity=1)  # create new empty transmitting queue
                yield self.env.timeout(self.times.ack_timeout)  # simulate ack timeout after failed transmission
                return False

        except simpy.Interrupt:  # this station does not have the longest frame, waiting frame time
            yield self.env.timeout(self.frame_to_send.frame_time)

        was_sent = self.check_collision()

        if was_sent:  # check if collision occurred
            log(self, f'Waiting for ACK time: {self.times.get_ack_frame_time()}')
            yield self.env.timeout(self.times.get_ack_frame_time())  # wait ack
        else:
            log(self, "waiting ack timeout slave")
            yield self.env.timeout(Times.ack_timeout)  # simulate ack timeout after failed transmission
        self.is_transmitting = False    
        return was_sent

    def check_collision(self):  # check if the collision occurred

        if (len(self.channel.tx_list) + len(self.channel.tx_list_NR)) > 1 or (len(self.channel.tx_list) + len(self.channel.tx_list_NR)) == 0:
            self.sent_failed()
            return False
        else:
            self.sent_completed()
            return True

    def generate_new_back_off_time(self, failed_transmissions_in_row):
        upper_limit = (pow(2, failed_transmissions_in_row) * (
                self.cw_min + 1) - 1)  # define the upper limit basing on  unsuccessful transmissions in the row
        upper_limit = (
            upper_limit if upper_limit <= self.cw_max else self.cw_max)  # set upper limit to CW Max if is bigger then this parameter
        back_off = random.randint(0, upper_limit)  # draw the back off value
        self.channel.backoffs[back_off][self.channel.n_of_stations] += 1  # store drawn value for future analyzes
        return back_off * self.times.t_slot

    def generate_new_frame(self):
        # frame_length = self.times.get_ppdu_frame_time()
        frame_length = 5400
        return Frame(frame_length, self.name, self.col, self.config.data_size, self.env.now)

    def sent_failed(self):
        log(self, "There was a collision")
        self.frame_to_send.number_of_retransmissions += 1
        self.channel.failed_transmissions += 1
        self.failed_transmissions += 1
        self.failed_transmissions_in_row += 1
        log(self, self.channel.failed_transmissions)
        if self.frame_to_send.number_of_retransmissions > self.config.r_limit:
            self.frame_to_send = self.generate_new_frame()
            self.failed_transmissions_in_row = 0

    def sent_completed(self):
        log(self, f"Successfully sent frame, waiting ack: {self.times.get_ack_frame_time()}")
        self.frame_to_send.t_end = self.env.now
        self.frame_to_send.t_to_send = (self.frame_to_send.t_end - self.frame_to_send.t_start)
        self.channel.succeeded_transmissions += 1
        self.succeeded_transmissions += 1
        self.failed_transmissions_in_row = 0
        self.channel.bytes_sent += self.frame_to_send.data_size
        self.channel.airtime_data[self.name] += self.frame_to_send.frame_time
        if len(self.packet_queue) > 0:
            packet = self.packet_queue.pop(0)
            latency = self.env.now - packet.gen_time
            self.total_latency += latency
            self.packet_count += 1
        return True
    def create_and_queue_packet(self):
        new_packet = Packet(id=self.next_packet_id, gen_time=self.env.now, payload_size=self.config.payload_size)
        self.packet_queue.append(new_packet)
        self.next_packet_id += 1

# Gnb -- mirroring station
class Gnb:
    def __init__(
            self,
            env: simpy.Environment,
            name: str,
            channel: dataclass,
            config_nr: Config_NR = Config_NR(),
    ):
        self.config_nr = config_nr
        # self.times = Times(config.data_size, config.mcs)  # using Times script to get time calculations
        self.name = name  # name of the station
        self.env = env  # simpy environment
        self.col = random.choice(colors)  # color of output -- for future station distinction
        self.transmission_to_send = None  # the transmision object which is next to send
        self.succeeded_transmissions = 0  # all succeeded transmissions for station
        self.failed_transmissions = 0  # all failed transmissions for station
        self.failed_transmissions_in_row = 0  # all failed transmissions for station in a row
        self.cw_min = config_nr.cw_min  # cw min parameter value
        self.N = None  # backoff counter
        self.desync = 0
        self.next_sync_slot_boundry = 0
        self.cw_max = config_nr.cw_max  # cw max parameter value
        self.channel = channel  # channel objfirst_transmission
        env.process(self.start())  # starting simulation process
        env.process(self.sync_slot_counter())
        self.process = None  # waiting back off process
        self.channel.airtime_data_NR.update({name: 0})
        self.channel.airtime_control_NR.update({name: 0})
        self.desync_done = False
        self.first_interrupt = False
        self.back_off_time = 0
        self.time_to_next_sync_slot = 0
        self.waiting_backoff = False
        self.start_nr = 0
        self.total_latency = 0  # Total latency for all packets
        self.packet_count = 0   # Number of packets successfully sent
        self.transmit_power_dbm = 23  # Transmit power in dBm (typically higher than Wi-Fi)
        self.is_transmitting = False

    def start(self):

        # yield self.env.timeout(self.desync)
        while True:
            # self.transmission_to_send = self.gen_new_transmission()
            was_sent = False
            while not was_sent:
                if gap:
                    self.process = self.env.process(self.wait_back_off_gap())
                    yield self.process
                    was_sent = yield self.env.process(self.send_transmission())
                else:
                    self.process = self.env.process(self.wait_back_off())
                    yield self.process
                    was_sent = yield self.env.process(self.send_transmission())

    def wait_back_off_gap(self):
        self.back_off_time = self.generate_new_back_off_time(self.failed_transmissions_in_row)
        # adding pp to the backoff timer
        m = self.config_nr.M
        prioritization_period_time = self.config_nr.deter_period + m * self.config_nr.observation_slot_duration
        self.back_off_time += prioritization_period_time  # add Priritization Period time to bacoff procedure

        while self.back_off_time > -1:
            try:
                with self.channel.tx_lock.request() as req:  # waiting  for idle channel -- empty channel
                    yield req

                self.time_to_next_sync_slot = self.next_sync_slot_boundry - self.env.now

                log(self, f'Backoff = {self.back_off_time} , and time to next slot: {self.time_to_next_sync_slot}')
                while self.back_off_time >= self.time_to_next_sync_slot:
                    self.time_to_next_sync_slot += self.config_nr.synchronization_slot_duration
                    log(self,
                        f'Backoff > time to sync slot: new time to next possible sync +1000 = {self.time_to_next_sync_slot}')

                gap_time = self.time_to_next_sync_slot - self.back_off_time
                log(self, f"Waiting gap period of : {gap_time} us")
                assert gap_time >= 0, "Gap period is < 0!!!"

                yield self.env.timeout(gap_time)
                log(self, f"Finished gap period")


                self.first_interrupt = True

                self.start_nr = self.env.now  # store the current simulation time

                log(self, f'Channels in use by {self.channel.tx_lock.count} stations')

                # checking if channel if idle
                if (len(self.channel.tx_list_NR) + len(self.channel.tx_list)) > 0:
                    log(self, 'Channel busy -- waiting to be free')
                    with self.channel.tx_lock.request() as req:
                        yield req
                    log(self, 'Finished waiting for free channel - restarting backoff procedure')

                else:
                    log(self, 'Channel free')
                    log(self, f"Starting to wait backoff: ({self.back_off_time}) us...")
                    self.channel.back_off_list_NR.append(self)  # join the list off stations which are waiting Back Offs
                    self.waiting_backoff = True

                    yield self.env.timeout(self.back_off_time)  # join the environment action queue

                    log(self, f"Backoff waited, sending frame...")
                    self.back_off_time = -1  # leave the loop
                    self.waiting_backoff = False

                    self.channel.back_off_list_NR.remove(
                        self)  # leave the waiting list as Backoff was waited successfully

            except simpy.Interrupt:  # handle the interruptions from transmitting stations
                log(self, "Waiting was interrupted")
                if self.first_interrupt and self.start is not None and self.waiting_backoff is True:
                    log(self, "Backoff was interrupted, waiting to resume backoff...")
                    already_waited = self.env.now - self.start_nr

                    if already_waited <= prioritization_period_time:
                        self.back_off_time -= prioritization_period_time
                        log(self, f"Interrupted in PP time {prioritization_period_time}, backoff {self.back_off_time}")
                    else:
                        slots_waited = int((already_waited - prioritization_period_time) / self.config_nr.observation_slot_duration)
                        # self.back_off_time -= already_waited  # set the Back Off to the remaining one
                        self.back_off_time -= ((slots_waited * self.config_nr.observation_slot_duration) + prioritization_period_time)
                        log(self, f"Completed slots(9us) {slots_waited} = {(slots_waited * self.config_nr.observation_slot_duration)}  plus PP time {prioritization_period_time}")
                        log(self, f"Backoff decresed by {(slots_waited * self.config_nr.observation_slot_duration) + prioritization_period_time} new Backoff {self.back_off_time}")

                    #log(self, f"already waited {already_waited} Backoff us, new Backoff {self.back_off_time}")
                    self.back_off_time += prioritization_period_time  # addnin new PP before next weiting
                    self.first_interrupt = False
                    self.waiting_backoff = False

    def wait_back_off(self):
        # Wait random number of slots N x OBSERVATION_SLOT_DURATION us
        global start
        self.back_off_time = self.generate_new_back_off_time(self.failed_transmissions_in_row)
        m = self.config_nr.M
        prioritization_period_time = self.config_nr.deter_period + m * self.config_nr.observation_slot_duration

        while self.back_off_time > -1:

            try:
                with self.channel.tx_lock.request() as req:  # waiting  for idle channel -- empty channel
                    yield req

                self.first_interrupt = True
                self.back_off_time += prioritization_period_time  # add Priritization Period time to bacoff procedure
                log(self, f"Starting to wait backoff (with PP): ({self.back_off_time}) us...")
                start = self.env.now  # store the current simulation time
                self.channel.back_off_list_NR.append(self)  # join the list off stations which are waiting Back Offs

                yield self.env.timeout(self.back_off_time)  # join the environment action queue

                log(self, f"Backoff waited, sending frame...")
                self.back_off_time = -1  # leave the loop

                self.channel.back_off_list_NR.remove(self)  # leave the waiting list as Backoff was waited successfully

            except simpy.Interrupt:  # handle the interruptions from transmitting stations
                log(self, "Backoff was interrupted, waiting to resume backoff...")
                if self.first_interrupt and start is not None:
                    already_waited = self.env.now - start

                    if already_waited <= prioritization_period_time:
                        self.back_off_time -= prioritization_period_time
                        log(self, f"Interrupted in PP time {prioritization_period_time}, backoff {self.back_off_time}")
                    else:
                        slots_waited = int((already_waited - prioritization_period_time) / self.config_nr.observation_slot_duration)
                        # self.back_off_time -= already_waited  # set the Back Off to the remaining one
                        self.back_off_time -= ((slots_waited * self.config_nr.observation_slot_duration) + prioritization_period_time)
                        log(self, f"Completed slots(9us) {slots_waited} = {(slots_waited * self.config_nr.observation_slot_duration)}  plus PP time {prioritization_period_time}")
                        log(self, f"Backoff decresed by {(slots_waited * self.config_nr.observation_slot_duration) + prioritization_period_time} new Backoff {self.back_off_time}")

                    self.first_interrupt = False
                    self.waiting_backoff = False

    def sync_slot_counter(self):
        # Process responsible for keeping the next sync slot boundry timestamp
        self.desync = random.randint(self.config_nr.min_sync_slot_desync, self.config_nr.max_sync_slot_desync)
        self.next_sync_slot_boundry = self.desync
        log(self, f"Selected random desync to {self.desync} us")
        yield self.env.timeout(self.desync)  # waiting randomly chosen desync time
        while True:
            self.next_sync_slot_boundry += self.config_nr.synchronization_slot_duration
            log(self, f"Next synch slot boundry is: {self.next_sync_slot_boundry}")
            yield self.env.timeout(self.config_nr.synchronization_slot_duration)

    def send_transmission(self):
        self.is_transmitting = True
        self.channel.tx_list_NR.append(self)  # add station to currently transmitting list
        self.transmission_to_send = self.gen_new_transmission()
        res = self.channel.tx_queue.request(priority=(
                big_num - self.transmission_to_send.transmission_time))  # create request basing on this station frame length

        try:
            result = yield res | self.env.timeout(
                0)  # try to hold transmitting lock(station with the longest frame will get this)

            if res not in result:  # check if this station got lock, if not just wait you frame time
                raise simpy.Interrupt("There is a longer frame...")

            with self.channel.tx_lock.request() as lock:  # this station has the longest frame so hold the lock
                yield lock

                for station in self.channel.back_off_list:  # stop all station which are waiting backoff as channel is not idle
                    if station.process.is_alive:
                        station.process.interrupt()
                for gnb in self.channel.back_off_list_NR:  # stop all station which are waiting backoff as channel is not idle
                    if gnb.process.is_alive:
                        gnb.process.interrupt()

                log(self, f'Transmission will be for: {self.transmission_to_send.transmission_time} time')

                yield self.env.timeout(self.transmission_to_send.transmission_time)

                self.channel.back_off_list_NR.clear()  # channel idle, clear backoff waiting list
                was_sent = self.check_collision()  # check if collision occurred

                if was_sent:  # transmission successful
                    self.channel.airtime_control_NR[self.name] += self.transmission_to_send.rs_time
                    log(self, f"adding rs time to control data: {self.transmission_to_send.rs_time}")
                    self.channel.airtime_data_NR[self.name] += self.transmission_to_send.airtime
                    log(self, f"adding data airtime to data: {self.transmission_to_send.airtime}")
                    self.channel.tx_list_NR.clear()  # clear transmitting list
                    self.channel.tx_list.clear()
                    self.channel.tx_queue.release(res)  # leave the transmitting queue
                    return True

            # there was collision
            self.channel.tx_list_NR.clear()  # clear transmitting list
            self.channel.tx_list.clear()
            self.channel.tx_queue.release(res)  # leave the transmitting queue
            self.channel.tx_queue = simpy.PreemptiveResource(self.env,
                                                             capacity=1)  # create new empty transmitting queue
            # yield self.env.timeout(self.times.ack_timeout)
            return False

        except simpy.Interrupt:  # this station does not have the longest frame, waiting frame time
            yield self.env.timeout(self.transmission_to_send.transmission_time)

        was_sent = self.check_collision()
        self.is_transmitting = False
        return was_sent

    def check_collision(self):  # check if the collision occurred

        if gap:
            # if (len(self.channel.tx_list) + len(self.channel.tx_list_NR)) > 1 and self.waiting_backoff is True:
            if (len(self.channel.tx_list) + len(self.channel.tx_list_NR)) > 1 or (len(self.channel.tx_list) + len(self.channel.tx_list_NR)) == 0:
                self.sent_failed()
                return False
            else:
                self.sent_completed()
                return True
        else:
            if (len(self.channel.tx_list) + len(self.channel.tx_list_NR)) > 1 or (len(self.channel.tx_list) + len(self.channel.tx_list_NR)) == 0:
                self.sent_failed()
                return False
            else:
                self.sent_completed()
                return True

    def gen_new_transmission(self):
        transmission_time = self.config_nr.mcot * 1000  # transforming to usec
        if gap:
            rs_time = 0
        else:
            rs_time = self.next_sync_slot_boundry - self.env.now
        airtime = transmission_time - rs_time
        return Transmission_NR(transmission_time, self.name, self.col, self.env.now, airtime, rs_time)

    def generate_new_back_off_time(self, failed_transmissions_in_row):
        # BACKOFF TIME GENERATION
        upper_limit = (pow(2, failed_transmissions_in_row) * (
                self.cw_min + 1) - 1)  # define the upper limit basing on  unsuccessful transmissions in the row
        upper_limit = (
            upper_limit if upper_limit <= self.cw_max else self.cw_max)  # set upper limit to CW Max if is bigger then this parameter
        back_off = random.randint(0, upper_limit)  # draw the back off value
        self.channel.backoffs[back_off][self.channel.n_of_stations] += 1  # store drawn value for future analyzes
        return back_off * self.config_nr.observation_slot_duration

    def sent_failed(self):
        log(self, "There was a collision")
        self.transmission_to_send.number_of_retransmissions += 1
        self.channel.failed_transmissions_NR += 1
        self.failed_transmissions += 1
        self.failed_transmissions_in_row += 1
        log(self, self.channel.failed_transmissions_NR)
        if self.transmission_to_send.number_of_retransmissions > 7:
            self.failed_transmissions_in_row = 0

    def sent_completed(self):
        log(self, f"Successfully sent transmission")
        self.transmission_to_send.t_end = self.env.now
        self.transmission_to_send.t_to_send = (self.transmission_to_send.t_end - self.transmission_to_send.t_start)
        self.channel.succeeded_transmissions_NR += 1
        self.succeeded_transmissions += 1
        self.failed_transmissions_in_row = 0
        return True


@dataclass()
class Channel:
    tx_queue: simpy.PreemptiveResource  # lock for the stations with the longest frame to transmit
    tx_lock: simpy.Resource  # channel lock (locked when there is ongoing transmission)
    n_of_stations: int  # number of transmitting stations in the channel
    n_of_eNB: int
    backoffs: Dict[int, Dict[int, int]]
    airtime_data: Dict[str, int]
    airtime_control: Dict[str, int]
    airtime_data_NR: Dict[str, int]
    airtime_control_NR: Dict[str, int]
    tx_list: List[Station] = field(default_factory=list)  # transmitting stations in the channel
    back_off_list: List[Station] = field(default_factory=list)  # stations in backoff phase
    tx_list_NR: List[Gnb] = field(default_factory=list)  # transmitting stations in the channel
    back_off_list_NR: List[Gnb] = field(default_factory=list)  # stations in backoff phase
    noise_floor_dbm: float = -95  # Noise floor in dBm
    stations: Dict[str, Station] = field(default_factory=dict)  # Store references to stations
    gnbs: Dict[str, Gnb] = field(default_factory=dict)  # Store references to gNBs

    failed_transmissions: int = 0  # total failed transmissions
    succeeded_transmissions: int = 0  # total succeeded transmissions
    bytes_sent: int = 0  # total bytes sent
    failed_transmissions_NR: int = 0  # total failed transmissions
    succeeded_transmissions_NR: int = 0  # total succeeded transmissions
    
    def calculate_sinr(self, node, all_nodes):
        """Calculate SINR for a given station or gNB."""
        # Default transmit power (if not set in node)
        tx_power_dbm = getattr(node, 'transmit_power_dbm', 20)
        
        # Convert to linear (mW)
        signal_power_mw = 10 ** (tx_power_dbm / 10)
        
        # Calculate interference from other nodes
        interference_power_mw = 0
        for other in all_nodes:
            if other != node:
                # Check if node is transmitting
                is_transmitting = (
                    (isinstance(other, Station) and other in self.tx_list) or
                    (isinstance(other, Gnb) and other in self.tx_list_NR)
                )
                
                if is_transmitting:
                    # Get transmit power (default to 20 dBm if not set)
                    other_tx_power_dbm = getattr(other, 'transmit_power_dbm', 20)
                    interference_power_mw += 10 ** (other_tx_power_dbm / 10)
        
        # Convert noise floor to mW
        noise_floor_mw = 10 ** (self.noise_floor_dbm / 10)
        
        # Total interference plus noise
        total_interference_mw = interference_power_mw + noise_floor_mw
        
        # Calculate SINR (linear)
        if total_interference_mw > 0:
            sinr_linear = signal_power_mw / total_interference_mw
            # Convert to dB
            sinr_db = 10 * math.log10(sinr_linear)
        else:
            sinr_db = 100  # Very high SINR if no interference
            
        return sinr_db
    
@dataclass
class Packet:
    id: int
    gen_time: float
    payload_size: int
    delivery_time: float = None
    

@dataclass()
class Frame:
    frame_time: int  # time of the frame
    station_name: str  # name of the owning it station
    col: str  # output color
    data_size: int  # payload size
    t_start: int  # generation time
    number_of_retransmissions: int = 0  # retransmissions count
    t_end: int = None  # sent time
    t_to_send: int = None  # how much time it took to sent successfully
    packet_gen_time: float = field(default_factory=lambda: -1.0)  # When packet was generated
    packet_size: int = field(default=1472)  # Packet size in bytes
    def __repr__(self):
        return (self.col + "Frame: start=%d, end=%d, frame_time=%d, retransmissions=%d"
                % (self.t_start, self.t_end, self.t_to_send, self.number_of_retransmissions)
                )


@dataclass()
class Transmission_NR:
    transmission_time: int
    enb_name: str  # name of the owning it station
    col: str
    t_start: int  # generation time / transmision start (including RS)
    airtime: int  # time spent on sending data
    rs_time: int  # time spent on sending reservation signal before data
    number_of_retransmissions: int = 0
    t_end: int = None  # sent time / transsmision end = start + rs_time + airtime
    t_to_send: int = None
    collided: bool = False  # true if transmission colided with another one
    packet_gen_time: float = field(default_factory=lambda: -1.0) # Timestamp when packet was generated
    packet_size: int = field(default=1472)

def run_simulation(
        number_of_stations: int,
        number_of_gnb: int,
        seed: int,
        simulation_time: int,
        config: Config,
        configNr: Config_NR,
        backoffs: Dict[int, Dict[int, int]],
        airtime_data: Dict[str, int],
        airtime_control: Dict[str, int],
        airtime_data_NR: Dict[str, int],
        airtime_control_NR: Dict[str, int],
        enable_dynamic_cw: bool = False, 
        csv_output_path: str = "results/output.csv" 
):
    random.seed(seed)
    environment = simpy.Environment()
    channel = Channel(
        simpy.PreemptiveResource(environment, capacity=1),
        simpy.Resource(environment, capacity=1),
        number_of_stations,
        number_of_gnb,
        backoffs,
        airtime_data,
        airtime_control,
        airtime_data_NR,
        airtime_control_NR,
        noise_floor_dbm=-95  # Default noise floor in dBm
    )
    
    # Store references to stations and gNBs for later metric calculations
    channel.stations = {}
    channel.gnbs = {}

    # Create stations
    for i in range(1, number_of_stations + 1):
        station = Station(environment, f"Station {i}", channel, config)
        channel.stations[f"Station {i}"] = station

    # Create gNBs
    for i in range(1, number_of_gnb + 1):
        gnb = Gnb(environment, f"Gnb {i}", channel, configNr)
        channel.gnbs[f"Gnb {i}"] = gnb
    controller = None
    if enable_dynamic_cw:
        controller = DynamicCWController(
            env=environment,
            channel=channel,
            wifi_config=config,
            nru_config=configNr,
            measurement_interval=1000000,  # 1 second
            adjustment_step=5,
            target_fairness=0.95
        )
        controller.start_monitoring()
        print(f"Dynamic CW adjustment enabled (target fairness: {controller.target_fairness})")

    # Run simulation
    environment.run(until=simulation_time * 1000000)  # Convert seconds to microseconds

    # Calculate collision probability
    if number_of_stations != 0:
        if(channel.failed_transmissions + channel.succeeded_transmissions) != 0:
            p_coll = channel.failed_transmissions / (channel.failed_transmissions + channel.succeeded_transmissions)
            p_coll_str = "{:.4f}".format(p_coll)
        else:
            p_coll = 0
            p_coll_str = "0"
    else:
        p_coll = 0
        p_coll_str = "0"

    if number_of_gnb != 0:
        if (channel.failed_transmissions_NR + channel.succeeded_transmissions_NR) != 0:
            p_coll_NR = channel.failed_transmissions_NR / (
                    channel.failed_transmissions_NR + channel.succeeded_transmissions_NR)
            p_coll_NR_str = "{:.4f}".format(p_coll_NR)
        else:
            p_coll_NR = 0
            p_coll_NR_str = "0"
    else:
        p_coll_NR = 0
        p_coll_NR_str = "0"

    # Calculate channel metrics
    time = simulation_time * 1000000  # Total simulation time in microseconds
    channel_occupancy_time = 0
    channel_efficiency = 0
    channel_occupancy_time_NR = 0
    channel_efficiency_NR = 0

    for i in range(1, number_of_stations + 1):
        station_name = f"Station {i}"
        channel_occupancy_time += channel.airtime_data[station_name] + channel.airtime_control[station_name]
        channel_efficiency += channel.airtime_data[station_name]

    for i in range(1, number_of_gnb + 1):
        gnb_name = f"Gnb {i}"
        channel_occupancy_time_NR += channel.airtime_data_NR[gnb_name] + channel.airtime_control_NR[gnb_name]
        channel_efficiency_NR += channel.airtime_data_NR[gnb_name]

    # Normalized metrics
    normalized_channel_occupancy_time = channel_occupancy_time / time
    normalized_channel_efficiency = channel_efficiency / time
    normalized_channel_occupancy_time_NR = channel_occupancy_time_NR / time
    normalized_channel_efficiency_NR = channel_efficiency_NR / time
    normalized_channel_occupancy_time_all = (channel_occupancy_time + channel_occupancy_time_NR) / time
    normalized_channel_efficiency_all = (channel_efficiency + channel_efficiency_NR) / time

    # Calculate throughput (in Mbps)
    wifi_data_rate = 866.7  # Example data rate for 802.11ac with 80MHz channel, 256-QAM
    nru_data_rate = 1200.0  # Example data rate for NR-U

    # Wi-Fi throughput based on airtime efficiency and data rate
    wifi_throughput = normalized_channel_efficiency * wifi_data_rate if number_of_stations > 0 else 0
    
    # NR-U throughput based on airtime efficiency and data rate
    nru_throughput = normalized_channel_efficiency_NR * nru_data_rate if number_of_gnb > 0 else 0
    
    # Total system throughput
    total_throughput = wifi_throughput + nru_throughput

    # Calculate latency
    wifi_latency = 0
    wifi_packet_count = 0
    
    for station_name, station in channel.stations.items():
        if hasattr(station, 'total_latency') and hasattr(station, 'packet_count'):
            wifi_latency += station.total_latency
            wifi_packet_count += station.packet_count
    
    avg_wifi_latency = wifi_latency / wifi_packet_count if wifi_packet_count > 0 else 0
    
    nru_latency = 0
    nru_packet_count = 0
    
    for gnb_name, gnb in channel.gnbs.items():
        if hasattr(gnb, 'total_latency') and hasattr(gnb, 'packet_count'):
            nru_latency += gnb.total_latency
            nru_packet_count += gnb.packet_count
    
    avg_nru_latency = nru_latency / nru_packet_count if nru_packet_count > 0 else 0

    # Calculate SINR (if enabled in channel)
    wifi_sinr = 0
    nru_sinr = 0
    
    if hasattr(channel, 'calculate_sinr'):
        # Calculate average SINR for Wi-Fi stations
        if number_of_stations > 0:
            wifi_sinr_sum = 0
            for station_name, station in channel.stations.items():
                wifi_sinr_sum += channel.calculate_sinr(station, list(channel.stations.values()) + list(channel.gnbs.values()))
            wifi_sinr = wifi_sinr_sum / number_of_stations
            
        # Calculate average SINR for NR-U gNBs
        if number_of_gnb > 0:
            nru_sinr_sum = 0
            for gnb_name, gnb in channel.gnbs.items():
                nru_sinr_sum += channel.calculate_sinr(gnb, list(channel.stations.values()) + list(channel.gnbs.values()))
            nru_sinr = nru_sinr_sum / number_of_gnb
    
    # Calculate traditional fairness index
    fairness = 0
    if normalized_channel_occupancy_time > 0 or normalized_channel_occupancy_time_NR > 0:
        fairness = (normalized_channel_occupancy_time_all**2) / (2 * (normalized_channel_occupancy_time**2 + normalized_channel_occupancy_time_NR**2))
    
    # Calculate Jain's fairness index
    jains_fairness = 0
    
    if number_of_stations > 0 or number_of_gnb > 0:
        # Get throughput for each station and gNB
        all_throughputs = []
        
        # Add Wi-Fi station throughputs
        for i in range(1, number_of_stations + 1):
            station_name = f"Station {i}"
            if station_name in channel.airtime_data:
                station_throughput = (channel.airtime_data[station_name] / time) * wifi_data_rate
                all_throughputs.append(station_throughput)
        
        # Add NR-U gNB throughputs
        for i in range(1, number_of_gnb + 1):
            gnb_name = f"Gnb {i}"
            if gnb_name in channel.airtime_data_NR:
                gnb_throughput = (channel.airtime_data_NR[gnb_name] / time) * nru_data_rate
                all_throughputs.append(gnb_throughput)
        
        # Calculate Jain's fairness index
        if len(all_throughputs) > 0:
            sum_throughput = sum(all_throughputs)
            sum_squared_throughput = sum(t*t for t in all_throughputs)
            if sum_squared_throughput > 0:
                jains_fairness = (sum_throughput ** 2) / (len(all_throughputs) * sum_squared_throughput)

    # Joint metric: combination of fairness and efficiency
    joint = fairness * normalized_channel_occupancy_time_all

    # Print results
    print(
        f"SEED = {seed} N_stations:={number_of_stations} N_gNB:={number_of_gnb}  CW_MIN = {config.cw_min} CW_MAX = {config.cw_max} "
        f"WiFi pcol:={p_coll_str} WiFi cot:={normalized_channel_occupancy_time:.6f} WiFi eff:={normalized_channel_efficiency:.5f} "
        f"gNB pcol:={p_coll_NR_str} gNB cot:={normalized_channel_occupancy_time_NR:.4f} gNB eff:={normalized_channel_efficiency_NR:.4f} "
        f"all cot:={normalized_channel_occupancy_time_all:.7f} all eff:={normalized_channel_efficiency_all:.5f}"
    )
    print(f"WiFi succ: {channel.succeeded_transmissions} fail: {channel.failed_transmissions}")
    print(f"NR-U succ: {channel.succeeded_transmissions_NR} fail: {channel.failed_transmissions_NR}")
    print(f"WiFi Throughput: {wifi_throughput:.2f} Mbps, NR-U Throughput: {nru_throughput:.2f} Mbps")
    
    if wifi_packet_count > 0:
        print(f"WiFi Average Latency: {avg_wifi_latency:.2f} µs")
    
    if nru_packet_count > 0:
        print(f"NR-U Average Latency: {avg_nru_latency:.2f} µs")
        
    print(f"Traditional Fairness: {fairness:.6f}")
    print(f"Jain's Fairness Index: {jains_fairness:.6f}")
    print(f"Joint Metric: {joint:.6f}")
    
    if hasattr(channel, 'calculate_sinr'):
        print(f"Average WiFi SINR: {wifi_sinr:.2f} dB, Average NR-U SINR: {nru_sinr:.2f} dB")
    
    if controller:
        adjustment_csv = output_csv.replace('.csv', '_adjustments.csv')
        with open(adjustment_csv, 'a', newline='') as f:
            writer = csv.DictWriter(f, fieldnames=[
                'seed', 'time', 'wifi_cw_min', 'wifi_cw_max', 
                'nru_cw_min', 'nru_cw_max', 'fairness', 
                'wifi_airtime', 'nru_airtime'
            ])
            if f.tell() == 0:
                writer.writeheader()
            
            for entry in controller.adjustment_history:
                entry['seed'] = seed
                writer.writerow(entry)
    # Save results to CSV
    wifi_access_delay = 0
    nru_access_delay = 0
    
    if number_of_stations > 0:
        # Estimate WiFi access delay based on CW and collision rate
        avg_wifi_backoff_slots = config.cw_min / 2  # Average of uniform distribution
        wifi_access_delay = avg_wifi_backoff_slots * 9  # 9 microseconds per slot
        # Add penalty for collisions (retransmissions increase delay)
        if p_coll > 0:
            wifi_access_delay *= (1 + p_coll * 2)  # Collisions double the delay
    
    if number_of_gnb > 0:
        # Estimate NR-U access delay
        avg_nru_backoff_slots = configNr.cw_min / 2
        nru_access_delay = avg_nru_backoff_slots * configNr.observation_slot_duration
        # Add prioritization period delay
        nru_access_delay += (configNr.deter_period + configNr.M * configNr.observation_slot_duration)
        # Add penalty for collisions
        if p_coll_NR > 0:
            nru_access_delay *= (1 + p_coll_NR * 2)
    
    # Print access delays
    if number_of_stations > 0:
        print(f"WiFi Average Access Delay: {wifi_access_delay:.2f} µs")
    
    if number_of_gnb > 0:
        print(f"NR-U Average Access Delay: {nru_access_delay:.2f} µs")
    
    # NOW UPDATE THE CSV WRITING TO INCLUDE ACCESS DELAYS:
    
    # Save results to CSV
    write_header = True
    if os.path.isfile(output_csv):
        write_header = False
    
    with open(csv_output_path, mode='a', newline="") as result_file:
        result_adder = csv.writer(result_file, delimiter=',', quotechar='"', quoting=csv.QUOTE_MINIMAL)

        if write_header:
            result_adder.writerow([
                "Seed", "WiFi_Nodes", "NRU_Nodes", "WiFi_CW_Min", "WiFi_CW_Max", "NRU_CW_Min", "NRU_CW_Max", 
                "WiFi_Throughput", "NRU_Throughput", "Total_Throughput", 
                "WiFi_PLR", "NRU_PLR", 
                "WiFi_Latency", "NRU_Latency",
                "WiFi_Access_Delay", "NRU_Access_Delay",  # ADDED THESE
                "WiFi_SINR", "NRU_SINR",
                "Traditional_Fairness", "Jains_Fairness", "Joint_Metric",
                "WiFi_COT", "NRU_COT", "Total_COT",
                "WiFi_Efficiency", "NRU_Efficiency", "Total_Efficiency"
            ])

        # Write results to CSV - ADDED ACCESS DELAYS
        result_adder.writerow([
            seed, number_of_stations, number_of_gnb, config.cw_min, config.cw_max, configNr.cw_min, configNr.cw_max,
            wifi_throughput, nru_throughput, total_throughput,
            p_coll, p_coll_NR,
            avg_wifi_latency, avg_nru_latency,
            wifi_access_delay, nru_access_delay,  # ADDED THESE
            wifi_sinr, nru_sinr,
            fairness, jains_fairness, joint,
            normalized_channel_occupancy_time, normalized_channel_occupancy_time_NR, normalized_channel_occupancy_time_all,
            normalized_channel_efficiency, normalized_channel_efficiency_NR, normalized_channel_efficiency_all
        ])

<?php

/**
 * @file
 * Class UnitCalendar
 * Handles querying and updating the availability information
 * relative to a single bookable unit.
 */

class UnitCalendar extends RoomsCalendar implements UnitCalendarInterface {

  /**
   * The default state for the room if it has no specific booking.
   *
   * @var int
   */
  protected $default_state;

  /**
   * Constructs a UnitCalendar instance.
   *
   * @param int $unit_id
   *   The bookable unit_id.
   */
  public function __construct($unit_id) {
    $this->unit_id = $unit_id;
    // Load the booking unit.
    $unit = rooms_unit_load($unit_id);

    // When deleting booking which unit was deleted before we don't have unit.
    $default_state = (is_object($unit)) ? $unit->default_state : ROOMS_AVAILABLE;
    $this->default_state = $default_state;
    $this->base_table = 'rooms_availability';
  }

  /**
   * {@inheritdoc}
   */
  public function removeEvents($events) {
    $events_to_delete = array();
    foreach ($events as $event) {
      // Break any locks.
      $event->unlock();
      // Set the events to the default state.
      $event->id = $this->default_state;

      $events_to_delete[] = $event;
    }
    $this->updateCalendar($events_to_delete);
  }

  /**
   * {@inheritdoc}
   */
  public function stateAvailability(DateTime $start_date, DateTime $end_date, array $states = array()) {
    // Get all states in the date range.
    $existing_states = $this->getStates($start_date, $end_date);
    // Look at the difference between existing states and states to check.
    $diff = array_diff($existing_states, $states);
    $valid = (count($diff) > 0) ? FALSE : TRUE;
    return $valid;
  }

  /**
   * {@inheritdoc}
   */
  public function getStates(DateTime $start_date, DateTime $end_date, $confirmed = FALSE) {

    $states = array();
    // Get the raw day results.
    $results = $this->getRawDayData($start_date, $end_date);
    foreach ($results[$this->unit_id] as $year => $months) {
      foreach ($months as $mid => $month) {
        foreach ($month['states'] as $state) {
          if ($state['state'] < 0 && !$confirmed) {
            $states[] = -1;
          }
          else {
            $states[] = $state['state'];
          }
        }
      }
    }
    $states = array_unique($states);
    return $states;
  }

  /**
   * {@inheritdoc}
   */
  public function getEvents(DateTime $start_date, DateTime $end_date) {
    // Get the raw day results.
    $results = $this->getRawDayData($start_date, $end_date);
    $events = array();
    foreach ($results[$this->unit_id] as $year => $months) {
      foreach ($months as $mid => $month) {
        // The event array returns the start days for each event within a month.
        $start_days = array_keys($month['states']);
        foreach ($month['states'] as $state) {
          // Create a booking event.
          $start = $state['start_day'];
          $end = $state['end_day'];
          $sd = new DateTime("$year-$mid-$start");
          $ed = new DateTime("$year-$mid-$end");

          $event = new BookingEvent($this->unit_id,
            $state['state'],
            $sd,
            $ed);
          $events[] = $event;
        }
      }
    }
    return $events;
  }

  /**
   * {@inheritdoc}
   */
  public function getRawDayData(DateTime $start_date, DateTime $end_date) {
    // Create a dummy BookingEvent to represent the range we are searching over.
    // This gives us access to handy functions that BookingEvents have.
    $s = new BookingEvent($this->unit_id, 0, $start_date, $end_date);
    $results = array();

    // Start by doing a query to the db to get any info stored there.
    // If search across the same year do a single query.
    if ($s->sameYear()) {
      $query = db_select('rooms_availability', 'a');
      $query->fields('a');
      $query->condition('a.unit_id', $this->unit_id);
      $query->condition('a.year', $s->startYear());
      $query->condition('a.month', $s->startMonth(), '>=');
      $query->condition('a.month', $s->endMonth(), '<=');
      $months = $query->execute()->fetchAll(PDO::FETCH_ASSOC);
      if (count($months) > 0) {
        foreach ($months as $month) {
          $m = $month['month'];
          $y = $month['year'];
          $id = $month['unit_id'];
          // Remove the three first rows and just keep the days.
          unset($month['month']);
          unset($month['year']);
          unset($month['unit_id']);
          $results[$id][$y][$m]['days'] = $month;
        }
      }
    }
    // For multiple years do a query for each year.
    else {
      for ($j = $s->startYear(); $j <= $s->endYear(); $j++) {
        $query = db_select('rooms_availability', 'a');
        $query->fields('a');
        $query->condition('a.unit_id', $this->unit_id);
        $query->condition('a.year', $j);
        if ($j == $s->startYear()) {
          $query->condition('a.month', $s->startMonth(), '>=');
        }
        elseif ($j == $s->endYear()) {
          $query->condition('a.month', $s->endMonth(), '<=');
        }
        $months = $query->execute()->fetchAll(PDO::FETCH_ASSOC);
        if (count($months) > 0) {
          foreach ($months as $month) {
            $m = $month['month'];
            $y = $month['year'];
            $id = $month['unit_id'];
            unset($month['month']);
            unset($month['year']);
            unset($month['unit_id']);
            $results[$id][$y][$m]['days'] = $month;
          }
        }
      }
    }

    // With the results from the db in place fill in any missing months
    // with the default state for the unit.
    for ($j = $s->startYear(); $j <= $s->endYear(); $j++) {
      $eod = rooms_end_of_month_dates($j);

      // We start by setting the expected start and end months for each year.
      if ($s->sameYear()) {
        $expected_months = $s->endMonth() - $s->startMonth() + 1;
        $sm = $s->startMonth();
        $em = $s->endMonth();
      }
      elseif ($j == $s->endYear()) {
        $expected_months = $s->endMonth();
        $sm = 1;
        $em = $s->endMonth();
      }
      elseif ($j == $s->startYear()) {
        $expected_months = 12 - $s->startMonth() + 1;
        $em = 12;
        $sm = $s->startMonth();
      }
      else {
        $expected_months = 12;
        $sm = 1;
        $em = 12;
      }

      // We check to see if the months we have already fit our expectations.
      $actual_months = isset($result[$this->unit_id][$j]) ? count($results[$id][$j]) : 0;
      if ($expected_months > $actual_months) {
        // We have missing months so lets go fill them.
        for ($i = $sm; $i <= $em; $i++) {
          if (!isset($results[$this->unit_id][$j][$i])) {
            $last_day = $eod[$i];
            $month = $this->prepareFullMonthArray(new BookingEvent($this->unit_id,
              $this->default_state,
              new DateTime("$j-$i-1"),
              new DateTime("$j-$i-$last_day")));
            // Add the month in its rightful position.
            $results[$this->unit_id][$j][$i]['days'] = $month;
            // And sort months.
            ksort($results[$this->unit_id][$j]);
          }
        }
      }
    }

    // With all the months in place we now need to clean results to set the
    // right start and end date for each month - this will save code downstream
    // from having to worry about it.
    foreach ($results[$this->unit_id] as $year => $months) {
      foreach ($months as $mid => $days) {
        // Get the end of month values again to make sure we have the right year
        // because it might change for queries spanning years.
        $eod = rooms_end_of_month_dates($year);
        // There is undoubtetly a smarter way to do the clean up below - but
        // will live with this for now.
        if (count($days['days']) != $eod[$mid]) {
          switch ($eod[$mid]) {
            case 30:
              unset($results[$this->unit_id][$year][$mid]['days']['d31']);
              break;

            case 29:
              unset($results[$this->unit_id][$year][$mid]['days']['d31']);
              unset($results[$this->unit_id][$year][$mid]['days']['d30']);
              break;

            case 28:
              unset($results[$this->unit_id][$year][$mid]['days']['d31']);
              unset($results[$this->unit_id][$year][$mid]['days']['d30']);
              unset($results[$this->unit_id][$year][$mid]['days']['d29']);
              break;
          }
        }
        if (($year == $s->startYear()) && ($mid == $s->startMonth())) {
          // We know we have the entire months over the range so we just unset
          // all the dates from the start of the month to the actual start day.
          for ($i = 1; $i < $s->startDay(); $i++) {
            unset($results[$this->unit_id][$year][$mid]['days']['d' . $i]);
          }
        }
        if (($year == $s->endYear()) && ($mid == $s->endMonth())) {
          // And from the end of the month back to the actual end day.
          for ($i = $s->endDay() + 1; $i <= $eod[$mid]; $i++) {
            unset($results[$this->unit_id][$year][$mid]['days']['d' . $i]);
          }
        }

      }
    }

    // With the results in place we do a states array with the start and
    // end dates of each event.
    foreach ($results[$this->unit_id] as $year => $months) {
      foreach ($months as $mid => $days) {
        // The number of days in the month we are interested in eventing.
        $j = count($days);
        // The start date.
        $i = substr(key($days['days']), 1);
        $start_day = $i;
        $end_day = NULL;
        $unique_states = array();
        $old_state = $days['days']['d' . $i];
        $state = $days['days']['d' . $i];
        while ($j <= count($days['days'])) {
          $state = $days['days']['d' . $i];
          if ($state != $old_state) {
            $unique_states[] = array(
              'state' => $old_state,
              'start_day' => $start_day,
              'end_day' => $i - 1,
            );
            $end_day = $i - 1;
            $start_day = $i;
            $old_state = $state;
          }
          $i++;
          $j++;
        }
        // Get the last event in.
        $unique_states[] = array(
          'state' => $state,
          'start_day' => isset($end_day) ? $end_day + 1 : $start_day,
          'end_day' => $i - 1,
        );
        $results[$this->unit_id][$year][$mid]['states'] = $unique_states;
      }
    }

    return $results;
  }

  /**
   * {@inheritdoc}
   */
  public function eventBlocked(BookingEventInterface $event) {

    $states = $this->getStates($event->start_date, $event->end_date);
    $blocked = FALSE;

    // Query the locks table to see if event is blocked.
    $query = db_select('rooms_booking_locks', 'l');
    $query->addField('l', 'unit_id');
    $query->addField('l', 'state');
    $query->addField('l', 'locked');
    $query->condition('l.unit_id', $this->unit_id);
    $query->condition('l.state', $states);
    $query->condition('l.locked', 1);
    $result = $query->execute()->fetchAll(PDO::FETCH_ASSOC);
    // Only block if we are trying to update an event that is not locked.
    if ((count($result) > 0) && $result[0]['state'] != $event->id) {
      $blocked = TRUE;
    }
    return $blocked;
  }

  /**
   * {@inheritdoc}
   */
  public function updateCalendar($events) {
    $response = array();
    // First check that none of the events supplied are blocked by an existing
    // event with a locked status.
    $monthly_events = array();

    foreach ($events as $event) {
      // Make sure event refers to the unit for this calendar.
      if ($event->unit_id == $this->unit_id) {
        // Check if event is not blocked by a locked event.
        if (!$this->eventBlocked($event)) {
          // If the event is in the same month span just queue to be added.
          if ($event->sameMonth()) {
            $monthly_events[] = $event;
          }
          else {
            // Check if multi-year - if not just create monthly events.
            if ($event->sameYear()) {
              $monthly_events_tmp = $event->transformToMonthlyEvents();
              $monthly_events = array_merge($monthly_events, $monthly_events_tmp);
            }
            else {
              // Else transform to single years and then to monthly.
              $yearly_events = $event->transformToYearlyEvents();
              foreach ($yearly_events as $ye) {
                $monthly_events_tmp = $ye->transformToMonthlyEvents();
                $monthly_events = array_merge($monthly_events, $monthly_events_tmp);
              }
            }
          }
        }
        else {
          $response[$event->id] = ROOMS_BLOCKED;
        }
      }
      else {
        $response[$event->id] = ROOMS_WRONG_UNIT;
      }
    }

    foreach ($monthly_events as $event) {
      $this->addMonthEvent($event);
      $response[$event->id] = ROOMS_UPDATED;
    }

    return $response;
  }

  /**
   * {@inheritdoc}
   */
  protected function prepareFullMonthArray(RoomsEventInterface $event) {
    $days = array();
    $eod = rooms_end_of_month_dates($event->startYear());
    $last_day = $eod[$event->startMonth()];

    for ($i = 1; $i <= $last_day; $i++) {
      if (($i >= $event->startDay()) && ($i <= $event->endDay())) {
        $days['d' . $i] = $event->id;
      }
      else {
        // Replace with default state.
        $days['d' . $i] = $this->default_state;
      }
    }
    return $days;
  }

  /**
   * {@inheritdoc}
   */
  protected function preparePartialMonthArray(RoomsEventInterface $event) {
    $days = array();
    for ($i = $event->startDay(); $i <= $event->endDay(); $i++) {
      $days['d' . $i] = $event->id;
    }
    return $days;
  }

  /**
   * {@inheritdoc}
   */
  public function getDefaultState() {
    return $this->default_state;
  }

}
